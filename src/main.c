#include "app.h"
#include "buffer.h"
#include "font.h"
#include "icons.h"
#include "image.h"
#include "lua_host.h"
#include "markdown.h"
#include "regex.h"
#include "vault.h"

#include <md4c-html.h>

#include <SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Pseudo-dir surfaced by the in-app folder picker for "Computer" / drive
 * enumeration. Defined up here so tinput_hit_test (which lives ahead of
 * the modal helpers) can reference it without a forward decl. */
#define COMPUTER_SENTINEL "::COMPUTER::"

#ifdef _WIN32
  #include <windows.h>
  #include <SDL_syswm.h>
#else
  #include <unistd.h>
#endif

#define DOWNSEE_VERSION "0.67.0"
#define MARGIN_X         36     /* doc inner padding; bumped for breathing room */
#define MARGIN_Y         20
#define INDENT_PX        22
#define SIDEBAR_PAD_X    10

/* Click hit kinds. The `kind` field on struct ClickHit (defined in app.h)
 * is one of these. */
enum ClickHitKind { HIT_WIKI, HIT_TASK };

/* Forward decl: user-keybinding loader, called from app_init early. */
static void user_kbinds_load_from_cfg(LuaHost* h);

/* True if the current theme has a light background (used so overlays can
 * pick a contrasting box bg). */
static int theme_is_light(const App* a)
{
    /* Average of bg RGB > 128 → light. */
    return (a->bg.r + a->bg.g + a->bg.b) / 3 > 128;
}

/* Slightly-different shade of the theme's bg for overlay boxes. Returns a
 * darker variant on dark themes and a lighter variant on light themes so
 * the overlay reads as elevated but stays in-theme. */
static SDL_Color overlay_bg(const App* a)
{
    SDL_Color c = a->bg;
    int delta = theme_is_light(a) ? -16 : +12;
    int r = (int)c.r + delta; if (r < 0) r = 0; if (r > 255) r = 255;
    int g = (int)c.g + delta; if (g < 0) g = 0; if (g > 255) g = 255;
    int b = (int)c.b + delta; if (b < 0) b = 0; if (b > 255) b = 255;
    return (SDL_Color){ (Uint8)r, (Uint8)g, (Uint8)b, 250 };
}

/* Dim everything behind an overlay. Single black slab at low alpha — the
 * SDL renderer composites it with the underlying frame. */
static void overlay_backdrop(App* a)
{
    SDL_SetRenderDrawColor(a->renderer, 0, 0, 0, 130);
    SDL_Rect bd = { 0, 0, a->win_w, a->win_h };
    SDL_RenderFillRect(a->renderer, &bd);
}

/* Cubic ease-out: smooth out the tail end of a 0→1 progress value so
 * motion feels like it settles rather than just stopping. Used by
 * render_chrome and other animated UI; full definition near app_render. */
static float ease_out_cubic(float t);

/* Forward decl: SDL_HitTest callback for borderless-window decorations.
 * Defined near the title bar code; app_init registers it. */
static SDL_HitTestResult SDLCALL window_hit_test_cb(SDL_Window* w,
    const SDL_Point* p, void* data);

#if defined(_WIN32)
/* Win32 WindowProc subclass that fixes aero snap on a borderless window:
 *  - WM_NCCALCSIZE returns 0 to make the client area span the full window
 *    once we re-add WS_THICKFRAME (otherwise the visible "frame" reappears).
 *  - WM_GETMINMAXINFO constrains the maximize size to the monitor work
 *    area so the window doesn't cover the taskbar.
 * Other messages chain to the original WndProc that SDL installed. */
static WNDPROC g_orig_wndproc    = NULL;
static App*    g_app_for_wndproc = NULL;
static LRESULT CALLBACK downsee_wndproc(HWND hwnd, UINT msg,
                                        WPARAM wp, LPARAM lp);
#endif

/* Baseline y for vertically-centering text in a row of height row_h.
 * Use this anywhere a text-in-a-row is rendered — leaves no top/bottom
 * imbalance when the row is taller than the glyph extent. Equivalent to:
 *   baseline = row_y + (row_h - line_height)/2 + ascent
 * Without this, callers default to row_y + ascent (top-aligned). */
static int row_text_baseline(Font* f, int row_y, int row_h)
{
    return row_y + (row_h - font_line_height(f)) / 2 + font_ascent(f);
}

/* Filled rounded rectangle. Routes through pill_draw (icons.c) which
 * rasterizes a rounded-rect SVG at 3x oversample and bilinear-downscales
 * for AA corners. The current SDL draw color is read and used as the
 * tint, so existing callers that do `SetRenderDrawColor` then `fill_rrect`
 * keep working unchanged — they just get smooth corners now. Cache
 * grows by one entry per distinct (w, h, radius). */
static void fill_rrect(SDL_Renderer* r, SDL_Rect rect, int radius)
{
    if (radius <= 0 || rect.w <= 0 || rect.h <= 0) {
        SDL_RenderFillRect(r, &rect);
        return;
    }
    Uint8 cr, cg, cb, ca;
    SDL_GetRenderDrawColor(r, &cr, &cg, &cb, &ca);
    SDL_Color c = { cr, cg, cb, ca };
    pill_draw(r, rect.x, rect.y, rect.w, rect.h, radius, c);
}

/* Outlined rounded rectangle (1 px stroke). Used for hairline edges. */
static void draw_rrect(SDL_Renderer* r, SDL_Rect rect, int radius)
{
    if (radius <= 0) { SDL_RenderDrawRect(r, &rect); return; }
    int x0 = rect.x, y0 = rect.y, w = rect.w, h = rect.h;
    int rad = radius;
    if (rad > w / 2) rad = w / 2;
    if (rad > h / 2) rad = h / 2;
    /* Straight edges (between corners). */
    SDL_RenderDrawLine(r, x0 + rad,     y0,         x0 + w - rad - 1, y0);
    SDL_RenderDrawLine(r, x0 + rad,     y0 + h - 1, x0 + w - rad - 1, y0 + h - 1);
    SDL_RenderDrawLine(r, x0,           y0 + rad,   x0,               y0 + h - rad - 1);
    SDL_RenderDrawLine(r, x0 + w - 1,   y0 + rad,   x0 + w - 1,       y0 + h - rad - 1);
    /* Corner arcs (Bresenham circle). */
    int dx = rad, dy = 0, err = 0;
    while (dx >= dy) {
        SDL_RenderDrawPoint(r, x0 + rad - dx,             y0 + rad - dy);
        SDL_RenderDrawPoint(r, x0 + rad - dy,             y0 + rad - dx);
        SDL_RenderDrawPoint(r, x0 + w - rad - 1 + dx,     y0 + rad - dy);
        SDL_RenderDrawPoint(r, x0 + w - rad - 1 + dy,     y0 + rad - dx);
        SDL_RenderDrawPoint(r, x0 + rad - dx,             y0 + h - rad - 1 + dy);
        SDL_RenderDrawPoint(r, x0 + rad - dy,             y0 + h - rad - 1 + dx);
        SDL_RenderDrawPoint(r, x0 + w - rad - 1 + dx,     y0 + h - rad - 1 + dy);
        SDL_RenderDrawPoint(r, x0 + w - rad - 1 + dy,     y0 + h - rad - 1 + dx);
        dy++;
        if (err <= 0) err += 2 * dy + 1;
        if (err > 0) { dx--; err -= 2 * dx + 1; }
    }
}

/* Shared "card" for every modal overlay: a soft drop-shadow rect 4px
 * down+right at low alpha, then the panel body filled with overlay_bg,
 * then a hairline edge in muted at very low alpha. Now rounded. */
#define CARD_RADIUS  6

static void overlay_card(App* a, SDL_Rect box)
{
    SDL_SetRenderDrawColor(a->renderer, 0, 0, 0, 90);
    SDL_Rect shadow = { box.x + 4, box.y + 4, box.w, box.h };
    fill_rrect(a->renderer, shadow, CARD_RADIUS);

    SDL_Color obg = overlay_bg(a);
    SDL_SetRenderDrawColor(a->renderer, obg.r, obg.g, obg.b, 250);
    fill_rrect(a->renderer, box, CARD_RADIUS);

    SDL_SetRenderDrawColor(a->renderer,
        a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 70);
    draw_rrect(a->renderer, box, CARD_RADIUS);
}

/* ----------------------------- theme presets ---------------------------- */
/* Each theme is a complete set of UI colors. The settings page cycles through
 * these and applies all 14 colors to the App on selection. The theme name
 * is what's persisted to init-overrides.lua. */
typedef struct {
    char name[32];
    SDL_Color bg, fg, fg_heading, fg_quote, fg_link;
    SDL_Color bg_code, fg_muted;
    SDL_Color bg_sidebar, bg_sidebar_hover, bg_sidebar_active;
    SDL_Color bg_status, fg_status;
    SDL_Color bg_selection, fg_cursor;
} Theme;

static const Theme g_themes[] = {
    /* 0: Editorial Dark — the new default. Warm grays + single copper-amber
     * accent. Reads like a magazine spread. */
    { "Editorial Dark",
        { 28,  26,  24, 255}, {230, 224, 212, 255}, {250, 245, 232, 255},
        {178, 142,  90, 255}, {206, 168, 116, 255},
        { 38,  35,  32, 255}, {145, 138, 125, 255},
        { 22,  20,  18, 255}, { 36,  33,  30, 255}, { 60,  50,  38, 255},
        { 18,  16,  14, 255}, {145, 138, 125, 255},
        {110,  80,  50, 180}, {240, 200, 140, 255},
    },
    /* 1: Default Dark — the original palette, kept for users who liked it. */
    { "Default Dark",
        { 24,  24,  28, 255}, {220, 220, 230, 255}, {245, 245, 255, 255},
        {110, 180, 200, 255}, {110, 170, 230, 255},
        { 34,  34,  40, 255}, {160, 165, 175, 255},
        { 18,  18,  22, 255}, { 34,  36,  44, 255}, { 50,  60,  82, 255},
        { 16,  16,  20, 255}, {160, 165, 175, 255},
        { 55,  85, 130, 180}, {240, 240, 250, 255},
    },
    /* 1: Light — easy on a bright office. */
    { "Light",
        {248, 248, 246, 255}, { 30,  30,  34, 255}, { 10,  10,  14, 255},
        { 60, 100, 130, 255}, { 30, 100, 170, 255},
        {236, 236, 232, 255}, {120, 122, 128, 255},
        {238, 238, 232, 255}, {220, 222, 220, 255}, {200, 215, 240, 255},
        {234, 234, 228, 255}, {120, 122, 128, 255},
        {180, 200, 235, 180}, { 30,  30,  34, 255},
    },
    /* 2: Solarized Dark. */
    { "Solarized Dark",
        {  0,  43,  54, 255}, {131, 148, 150, 255}, {238, 232, 213, 255},
        {133, 153,   0, 255}, { 38, 139, 210, 255},
        {  7,  54,  66, 255}, {101, 123, 131, 255},
        {  7,  54,  66, 255}, { 20,  72,  87, 255}, { 38,  90, 110, 255},
        {  0,  30,  39, 255}, {101, 123, 131, 255},
        { 38,  90, 130, 180}, {253, 246, 227, 255},
    },
    /* 3: Nord. */
    { "Nord",
        { 46,  52,  64, 255}, {216, 222, 233, 255}, {236, 239, 244, 255},
        {163, 190, 140, 255}, {136, 192, 208, 255},
        { 59,  66,  82, 255}, {136, 144, 162, 255},
        { 36,  41,  51, 255}, { 59,  66,  82, 255}, { 76,  86, 106, 255},
        { 30,  35,  44, 255}, {136, 144, 162, 255},
        { 76,  86, 106, 180}, {236, 239, 244, 255},
    },
    /* 4: Gruvbox Dark. */
    { "Gruvbox Dark",
        { 40,  40,  40, 255}, {235, 219, 178, 255}, {251, 241, 199, 255},
        {184, 187,  38, 255}, {131, 165, 152, 255},
        { 60,  56,  54, 255}, {168, 153, 132, 255},
        { 29,  32,  33, 255}, { 60,  56,  54, 255}, { 80,  73,  69, 255},
        { 24,  24,  24, 255}, {168, 153, 132, 255},
        { 80,  73,  69, 180}, {251, 241, 199, 255},
    },
    /* 6: Rose Pine Moon — warm but cool — soft rosy violets. */
    { "Rose Pine Moon",
        { 35,  33,  54, 255}, {224, 222, 244, 255}, {232, 230, 247, 255},
        {235, 188, 186, 255}, {196, 167, 231, 255},
        { 47,  45,  68, 255}, {144, 140, 170, 255},
        { 25,  23,  36, 255}, { 47,  45,  68, 255}, { 86,  82, 110, 255},
        { 22,  20,  32, 255}, {144, 140, 170, 255},
        { 86,  72, 130, 180}, {234, 154, 151, 255},
    },
};
#define G_THEME_COUNT ((int)(sizeof g_themes / sizeof g_themes[0]))

/* Look up a theme by name (case-insensitive); -1 if not found. */
static int theme_find(const char* name)
{
    if (!name) return -1;
    for (int i = 0; i < G_THEME_COUNT; ++i) {
        const char* a = g_themes[i].name;
        const char* b = name;
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
            if (ca != cb) break;
            a++; b++;
        }
        if (*a == 0 && *b == 0) return i;
    }
    return -1;
}

/* Forward decl. */
typedef struct App App_;     /* keep gcc happy if App not yet pulled in */
static void theme_apply(App* a, int idx)
{
    if (idx < 0 || idx >= G_THEME_COUNT) return;
    const Theme* t = &g_themes[idx];
    a->bg = t->bg; a->fg = t->fg;
    a->fg_heading = t->fg_heading; a->fg_quote = t->fg_quote;
    a->fg_link = t->fg_link;
    a->bg_code = t->bg_code; a->fg_muted = t->fg_muted;
    a->bg_sidebar = t->bg_sidebar;
    a->bg_sidebar_hover = t->bg_sidebar_hover;
    a->bg_sidebar_active = t->bg_sidebar_active;
    a->bg_status = t->bg_status; a->fg_status = t->fg_status;
    a->bg_selection = t->bg_selection; a->fg_cursor = t->fg_cursor;
}

/* ----------------------------- font choices ----------------------------- */
/* A small registry of known TrueType fonts the user can pick from in the
 * settings page. Populated at startup by checking which paths actually
 * exist on disk. Add more entries below to extend the list. */
typedef struct { char name[64]; char path[260]; } FontChoice;
static FontChoice g_font_choices[64];
static int        g_font_choice_count;

static int file_exists(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void font_choices_init(void)
{
    static const struct { const char* name; const char* path; } known[] = {
        /* Monospace — best for editor */
        {"Consolas",         "C:/Windows/Fonts/consola.ttf"},
        {"Cascadia Code",    "C:/Windows/Fonts/CascadiaCode.ttf"},
        {"Cascadia Mono",    "C:/Windows/Fonts/CascadiaMono.ttf"},
        {"Lucida Console",   "C:/Windows/Fonts/lucon.ttf"},
        {"Courier New",      "C:/Windows/Fonts/cour.ttf"},
        /* Proportional — readable for body text */
        {"Segoe UI",         "C:/Windows/Fonts/segoeui.ttf"},
        {"Calibri",          "C:/Windows/Fonts/calibri.ttf"},
        {"Verdana",          "C:/Windows/Fonts/verdana.ttf"},
        {"Tahoma",           "C:/Windows/Fonts/tahoma.ttf"},
        {"Arial",            "C:/Windows/Fonts/arial.ttf"},
        {"Times New Roman",  "C:/Windows/Fonts/times.ttf"},
        {"Georgia",          "C:/Windows/Fonts/georgia.ttf"},
        /* Drop bundled fonts in data/fonts/ — they're picked up here too. */
        {"Bundled: Inter",        "data/fonts/Inter-Regular.ttf"},
        {"Bundled: JetBrains",    "data/fonts/JetBrainsMono-Regular.ttf"},
        {"Bundled: SourceCodePro","data/fonts/SourceCodePro-Regular.ttf"},
        {NULL, NULL},
    };
    g_font_choice_count = 0;
    for (int i = 0; known[i].path; ++i) {
        if (!file_exists(known[i].path)) continue;
        if (g_font_choice_count >= (int)(sizeof g_font_choices / sizeof g_font_choices[0])) break;
        snprintf(g_font_choices[g_font_choice_count].name,
                 sizeof g_font_choices[0].name, "%s", known[i].name);
        snprintf(g_font_choices[g_font_choice_count].path,
                 sizeof g_font_choices[0].path, "%s", known[i].path);
        g_font_choice_count++;
    }
    fprintf(stderr, "downsee: %d font choices found\n", g_font_choice_count);
}

/* Find an index in g_font_choices matching `path`, or -1 if not present. */
static int font_choice_find(const char* path)
{
    for (int i = 0; i < g_font_choice_count; ++i)
        if (strcmp(g_font_choices[i].path, path) == 0) return i;
    return -1;
}

/* (Re)load every font according to the live App.cfg_* settings. Frees any
 * previously loaded font handles. Returns 0 on success, -1 if any font
 * fails to load. Reapplies the user's fallback chain after loading. */
static int app_reload_fonts(App* a)
{
    /* Free old. */
    if (a->font_body)             font_destroy(a->font_body);
    if (a->font_body_bold)        font_destroy(a->font_body_bold);
    if (a->font_body_italic)      font_destroy(a->font_body_italic);
    if (a->font_body_bold_italic) font_destroy(a->font_body_bold_italic);
    if (a->font_h1)               font_destroy(a->font_h1);
    if (a->font_h2)               font_destroy(a->font_h2);
    if (a->font_h3)               font_destroy(a->font_h3);
    if (a->font_code)             font_destroy(a->font_code);
    if (a->font_code_bold)        font_destroy(a->font_code_bold);
    if (a->font_code_italic)      font_destroy(a->font_code_italic);
    if (a->font_code_bold_italic) font_destroy(a->font_code_bold_italic);
    a->font_body = a->font_body_bold = a->font_body_italic =
        a->font_body_bold_italic = NULL;
    a->font_h1 = a->font_h2 = a->font_h3 = NULL;
    a->font_code = a->font_code_bold = a->font_code_italic =
        a->font_code_bold_italic = NULL;

    const char* fp  = a->cfg_font_path;
    const char* fpm = a->cfg_font_path_mono;
    int sz  = a->cfg_font_size;
    int sh1 = a->cfg_font_size_h1;
    int sh2 = a->cfg_font_size_h2;
    int sh3 = a->cfg_font_size_h3;

    a->font_body              = font_create(a->renderer, fp, sz,  FONT_STYLE_REGULAR);
    a->font_body_bold         = font_create(a->renderer, fp, sz,  FONT_STYLE_BOLD);
    a->font_body_italic       = font_create(a->renderer, fp, sz,  FONT_STYLE_ITALIC);
    a->font_body_bold_italic  = font_create(a->renderer, fp, sz,  FONT_STYLE_BOLD_ITALIC);
    a->font_h1                = font_create(a->renderer, fp, sh1, FONT_STYLE_REGULAR);
    a->font_h2                = font_create(a->renderer, fp, sh2, FONT_STYLE_REGULAR);
    a->font_h3                = font_create(a->renderer, fp, sh3, FONT_STYLE_REGULAR);
    a->font_code              = font_create(a->renderer, fpm, sz, FONT_STYLE_REGULAR);
    a->font_code_bold         = font_create(a->renderer, fpm, sz, FONT_STYLE_BOLD);
    a->font_code_italic       = font_create(a->renderer, fpm, sz, FONT_STYLE_ITALIC);
    a->font_code_bold_italic  = font_create(a->renderer, fpm, sz, FONT_STYLE_BOLD_ITALIC);
    if (!a->font_body || !a->font_body_bold || !a->font_body_italic ||
        !a->font_body_bold_italic || !a->font_h1 || !a->font_h2 ||
        !a->font_h3 || !a->font_code || !a->font_code_bold ||
        !a->font_code_italic || !a->font_code_bold_italic) {
        fprintf(stderr, "app_reload_fonts: font_create failed (%s, %s)\n", fp, fpm);
        return -1;
    }
    /* Helper: add a fallback font to every face in the chain. Skips silently
     * if the file doesn't exist (font.c logs to stderr internally). */
    #define ADD_FB(path_)                                                   \
        do {                                                                \
            font_add_fallback(a->font_body,              (path_));          \
            font_add_fallback(a->font_body_bold,         (path_));          \
            font_add_fallback(a->font_body_italic,       (path_));          \
            font_add_fallback(a->font_body_bold_italic,  (path_));          \
            font_add_fallback(a->font_h1,                (path_));          \
            font_add_fallback(a->font_h2,                (path_));          \
            font_add_fallback(a->font_h3,                (path_));          \
            font_add_fallback(a->font_code,              (path_));          \
            font_add_fallback(a->font_code_bold,         (path_));          \
            font_add_fallback(a->font_code_italic,       (path_));          \
            font_add_fallback(a->font_code_bold_italic,  (path_));          \
        } while (0)

    /* User-configured fallbacks first (init.lua font_fallback array). */
    int n_fb = lua_host_cfg_array_length(a->lua, "font_fallback");
    for (int i = 1; i <= n_fb; ++i) {
        const char* fb = lua_host_cfg_array_string(a->lua, "font_fallback", i);
        if (fb) ADD_FB(fb);
    }

    /* System fallbacks for codepoints the primary likely lacks (CJK,
     * symbols, emoji). These cover ~99% of "tofu box" reports. Files that
     * don't exist are skipped by font.c. Order matters: we want the broadest
     * symbol/ideograph coverage to come BEFORE color emoji because some
     * CJK fonts also include arrows/punctuation we'd rather render
     * monochrome. */
#if defined(_WIN32)
    /* Modern Windows ships these in C:/Windows/Fonts. */
    ADD_FB("C:/Windows/Fonts/seguisym.ttf");      /* Segoe UI Symbol */
    ADD_FB("C:/Windows/Fonts/YuGothM.ttc");       /* Yu Gothic Medium (CJK) */
    ADD_FB("C:/Windows/Fonts/msyh.ttc");          /* Microsoft YaHei (CJK) */
    ADD_FB("C:/Windows/Fonts/seguiemj.ttf");      /* Segoe UI Emoji (color) */
#elif defined(__APPLE__)
    ADD_FB("/System/Library/Fonts/Apple Symbols.ttf");
    ADD_FB("/System/Library/Fonts/PingFang.ttc");
    ADD_FB("/System/Library/Fonts/Apple Color Emoji.ttc");
#else
    ADD_FB("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    ADD_FB("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
    ADD_FB("/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf");
#endif
    #undef ADD_FB
    return 0;
}

static char* slurp(const char* path, size_t* out_len)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char* buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    if (out_len) *out_len = got;
    return buf;
}

static SDL_Color color_from_cfg(LuaHost* L, const char* key,
                                unsigned char dr, unsigned char dg,
                                unsigned char db, unsigned char da)
{
    unsigned char rgba[4];
    lua_host_cfg_color(L, key, dr, dg, db, da, rgba);
    return (SDL_Color){ rgba[0], rgba[1], rgba[2], rgba[3] };
}

static int doc_x_left(const App* a) { return a->sidebar_open ? a->sidebar_w : 0; }

/* Add the user's line-spacing override to a font's natural line height.
 * Used by every render-time line stepping so the user can loosen the
 * editor without changing font sizes. */
static int line_step(const App* a, Font* f)
{
    return font_line_height(f) + a->cfg_line_spacing;
}

/* Forward decl needed by settings_adjust (live preview of line spacing). */
static void ensure_cursor_visible(App* a);

/* Right edge of the document area: pulls in by the outline panel width
 * when the outline is pinned to the right. */
static int doc_x_right(const App* a)
{
    return a->win_w - (a->outline_pinned ? a->outline_panel_w : 0);
}

/* Custom title bar (drawn ourselves now that the window is borderless).
 * Holds the app icon, menu items (File/Edit/View/Help), filename text, and
 * min/max/close window controls on the right edge. */
static int title_bar_h(const App* a) { (void)a; return 32; }

/* Combined height of the top chrome strip = title bar + tool row. Lots of
 * layout code uses this as "where the doc / sidebar starts", so growing it
 * to include the title bar shifts everything below correctly. */
static int chrome_bar_h(const App* a)
{
    return title_bar_h(a) + font_line_height(a->font_body) + 16;
}

/* Height of the inner chrome row (below the title bar): breadcrumb, mode
 * pill, tool icons. Equals the OLD chrome_bar_h before the title bar was
 * folded in. Used for centering and button-cell sizing inside that row. */
static int chrome_row_h(const App* a)
{
    return chrome_bar_h(a) - title_bar_h(a);
}

static int status_bar_h(const App* a)
{
    return font_line_height(a->font_body) + 6;     /* slimmer in v0.31 */
}

/* Top y at which document content begins, after the chrome bar + a bit of
 * internal padding. Replaces every previous bare `MARGIN_Y - scroll_y`. */
static int doc_y_top(const App* a)
{
    return chrome_bar_h(a) + MARGIN_Y;
}

static int viewport_h(const App* a)
{
    return a->win_h - status_bar_h(a) - chrome_bar_h(a) - 2 * MARGIN_Y;
}

static int max_scroll(const App* a)
{
    int over = a->doc_height_px - viewport_h(a);
    return over > 0 ? over : 0;
}

static void clamp_scroll(App* a)
{
    if (a->scroll_y < 0) a->scroll_y = 0;
    int m = max_scroll(a);
    if (a->scroll_y > m) a->scroll_y = m;
}

static void update_window_title(App* a)
{
    char title[512];
    const char* name = a->note_path ? vault_basename(a->note_path) : "(untitled)";
    /* Prefer frontmatter `title:` when present so the window title matches
     * what the user actually called the note. Falls back to filename. */
    const char* shown = (a->fm_present && a->fm_title[0]) ? a->fm_title : name;
    snprintf(title, sizeof title, "Downsee " DOWNSEE_VERSION " \xe2\x80\x94 %s%s",
             shown, a->buf.dirty ? " *" : "");
    SDL_SetWindowTitle(a->window, title);
}

/* Called by the Lua host when a plugin invokes downsee.notify(s). Stores
 * the message and an expiry time; the status bar shows it until expiry. */
static void on_lua_notify(void* userdata, const char* msg)
{
    App* a = userdata;
    free(a->notification_msg);
    a->notification_msg   = strdup(msg);
    a->notification_until = SDL_GetTicks() + 3500;
}

/* Forward decl — info_modal pumps SDL events like confirm_action does. */
static void info_modal(App* a, const char* title, const char* msg);
static void on_lua_dialog(void* userdata,
                          const char* title, const char* msg)
{
    info_modal((App*)userdata, title, msg);
}

/* Reset the cursor-blink anchor so the cursor is visible the moment after
 * any user action (typing, motion, mode switch). */
static void bump_blink(App* a) { a->blink_anchor = SDL_GetTicks(); }

#define BLINK_PERIOD_MS 530
static bool cursor_visible_now(const App* a)
{
    uint32_t elapsed = SDL_GetTicks() - a->blink_anchor;
    return ((elapsed / BLINK_PERIOD_MS) % 2) == 0;
}

/* ----------------------------- frontmatter ------------------------------ */
/* Tiny YAML-ish parser for the leading `---` block at the top of a note.
 * Supports `key: value` lines, quoted strings, and `tags: [a, b]` /
 * multi-line `- item` arrays. Anything more (nested objects, anchors,
 * multi-line scalars) is ignored. */

/* If `data` starts with a `---` line, find the closing `---` line and write
 * the byte index of the first body byte into out_body_start. Returns 1 if a
 * frontmatter block is present, 0 otherwise. */
static int frontmatter_scan(const char* data, size_t len,
                            size_t* out_fm_start, size_t* out_fm_end,
                            size_t* out_body_start)
{
    if (len < 4 || memcmp(data, "---", 3) != 0) return 0;
    if (data[3] != '\n' && data[3] != '\r') return 0;
    size_t fm_start = 3;
    if (data[3] == '\r' && fm_start + 1 < len && data[fm_start + 1] == '\n')
        fm_start += 2;
    else
        fm_start += 1;

    size_t i = fm_start;
    while (i < len) {
        size_t ls = i;
        if (i + 3 <= len && memcmp(data + i, "---", 3) == 0) {
            char after = (i + 3 < len) ? data[i + 3] : 0;
            if (after == '\n' || after == '\r' || after == 0) {
                size_t end = i + 3;
                if (end < len && data[end] == '\r') end++;
                if (end < len && data[end] == '\n') end++;
                if (out_fm_start)   *out_fm_start   = fm_start;
                if (out_fm_end)     *out_fm_end     = ls;
                if (out_body_start) *out_body_start = end;
                return 1;
            }
        }
        while (i < len && data[i] != '\n') i++;
        if (i < len) i++;
    }
    return 0;
}

/* Look up a top-level `key: value` in the frontmatter range [fm..fm+fm_len).
 * Trims surrounding whitespace and matched single/double quotes from the
 * value. Returns 1 on hit (writes NUL-terminated `out`), 0 otherwise. */
static int fm_get_string(const char* fm, size_t fm_len, const char* key,
                         char* out, size_t cap)
{
    size_t key_len = strlen(key);
    size_t i = 0;
    while (i < fm_len) {
        size_t ls = i;
        while (i < fm_len && fm[i] != '\n') i++;
        size_t le = i;
        if (i < fm_len) i++;
        if (le > ls && fm[le - 1] == '\r') le--;
        if (le < ls + key_len + 1) continue;
        if (memcmp(fm + ls, key, key_len) != 0) continue;
        if (fm[ls + key_len] != ':') continue;
        size_t vs = ls + key_len + 1;
        while (vs < le && (fm[vs] == ' ' || fm[vs] == '\t')) vs++;
        size_t vlen = le - vs;
        while (vlen > 0 && (fm[vs + vlen - 1] == ' ' ||
                            fm[vs + vlen - 1] == '\t')) vlen--;
        if (vlen >= 2 &&
            ((fm[vs] == '"'  && fm[vs + vlen - 1] == '"') ||
             (fm[vs] == '\'' && fm[vs + vlen - 1] == '\'')))
        {
            vs++; vlen -= 2;
        }
        if (vlen >= cap) vlen = cap - 1;
        memcpy(out, fm + vs, vlen);
        out[vlen] = 0;
        return 1;
    }
    return 0;
}

/* Iterate every tag in the frontmatter `tags:` field. Supports flow form
 * `tags: [a, b]` and block form `tags:\n  - a\n  - b`. The callback is
 * called once per tag with a non-NUL-terminated slice + length. */
typedef void (*FmTagCb)(const char* tag, size_t tlen, void* ud);

static void fm_each_tag(const char* fm, size_t fm_len, FmTagCb cb, void* ud)
{
    size_t i = 0;
    while (i < fm_len) {
        size_t ls = i;
        while (i < fm_len && fm[i] != '\n') i++;
        size_t le = i;
        if (i < fm_len) i++;
        if (le > ls && fm[le - 1] == '\r') le--;
        if (le < ls + 5) continue;
        if (memcmp(fm + ls, "tags:", 5) != 0) continue;
        size_t vs = ls + 5;
        while (vs < le && (fm[vs] == ' ' || fm[vs] == '\t')) vs++;
        if (vs < le && fm[vs] == '[') {
            vs++;
            while (vs < le && fm[vs] != ']') {
                while (vs < le && (fm[vs] == ' ' || fm[vs] == '\t' ||
                                   fm[vs] == ',')) vs++;
                /* Strip an optional quote. */
                char q = 0;
                if (vs < le && (fm[vs] == '"' || fm[vs] == '\'')) {
                    q = fm[vs]; vs++;
                }
                size_t ts = vs;
                while (vs < le && fm[vs] != ',' && fm[vs] != ']' &&
                       (q ? fm[vs] != q : (fm[vs] != ' ' && fm[vs] != '\t')))
                    vs++;
                if (vs > ts && cb) cb(fm + ts, vs - ts, ud);
                if (q && vs < le && fm[vs] == q) vs++;
            }
            return;
        }
        /* Block form: scan subsequent indented `- name` lines. */
        while (i < fm_len) {
            size_t lls = i;
            while (i < fm_len && fm[i] != '\n') i++;
            size_t lle = i;
            if (i < fm_len) i++;
            if (lle > lls && fm[lle - 1] == '\r') lle--;
            size_t p = lls;
            while (p < lle && (fm[p] == ' ' || fm[p] == '\t')) p++;
            if (p == lle) continue;
            if (fm[p] != '-') return;
            p++;
            while (p < lle && (fm[p] == ' ' || fm[p] == '\t')) p++;
            /* Strip quotes. */
            if (p < lle && (fm[p] == '"' || fm[p] == '\'')) {
                char q = fm[p++];
                size_t ts = p;
                while (p < lle && fm[p] != q) p++;
                if (p > ts && cb) cb(fm + ts, p - ts, ud);
            } else {
                if (p < lle && cb) cb(fm + p, lle - p, ud);
            }
        }
        return;
    }
}

/* Append a tag to App.fm_tags_csv as a space-separated list. Truncates
 * silently if the buffer is full so we never overflow. */
static void fm_tag_to_csv_cb(const char* t, size_t n, void* ud)
{
    App* a = (App*)ud;
    size_t cur = strlen(a->fm_tags_csv);
    size_t cap = sizeof a->fm_tags_csv;
    size_t need = cur + (cur ? 1 : 0) + n + 1;
    if (need >= cap) return;
    if (cur > 0) a->fm_tags_csv[cur++] = ' ';
    memcpy(a->fm_tags_csv + cur, t, n);
    a->fm_tags_csv[cur + n] = 0;
}

static void reparse_preview(App* a)
{
    md_doc_free(&a->doc);

    /* Detect frontmatter and parse only the body so md4c doesn't try to
     * render `---` as an HR or process the YAML as paragraphs. The doc's
     * task_mark_off is sourced from md4c offsets, so for task-list lines we
     * patch the offset back into buf.data after parsing. */
    a->fm_present     = false;
    a->fm_body_start  = 0;
    a->fm_title[0]    = 0;
    a->fm_tags_csv[0] = 0;

    size_t fm_start = 0, fm_end = 0, body_start = 0;
    if (frontmatter_scan(a->buf.data, a->buf.len,
                         &fm_start, &fm_end, &body_start))
    {
        a->fm_present    = true;
        a->fm_body_start = body_start;
        size_t fm_len = fm_end - fm_start;
        const char* fm = a->buf.data + fm_start;
        fm_get_string(fm, fm_len, "title", a->fm_title, sizeof a->fm_title);
        if (!*a->fm_title)
            fm_get_string(fm, fm_len, "name", a->fm_title, sizeof a->fm_title);

        /* Build a space-separated csv of tag names so the render code can
         * walk it without re-parsing each frame. */
        fm_each_tag(fm, fm_len, fm_tag_to_csv_cb, a);
    }

    md_doc_parse(a->buf.data + body_start,
                 a->buf.len - body_start, &a->doc);

    /* Patch task offsets back into buf.data coordinates. */
    if (body_start > 0) {
        for (size_t i = 0; i < a->doc.line_count; ++i) {
            MdLine* ln = &a->doc.lines[i];
            if ((ln->kind == LINE_LIST_TASK_OPEN ||
                 ln->kind == LINE_LIST_TASK_DONE) && ln->task_mark_off > 0)
            {
                ln->task_mark_off += body_start;
            }
        }
    }
}

/* Move (or insert) `path` to the front of the recent_paths MRU list,
 * dropping the oldest entry if the list is full. Strings are owned by
 * the App; duplicates are detected case-insensitively. */
static void recent_push(App* a, const char* path)
{
    if (!path || !*path) return;
    if (strcmp(path, "(unsaved)") == 0) return;
    int cap = (int)(sizeof a->recent_paths / sizeof a->recent_paths[0]);
    int existing = -1;
    for (int i = 0; i < a->recent_count; ++i) {
        if (strcmp(a->recent_paths[i], path) == 0) { existing = i; break; }
    }
    char* moved = NULL;
    if (existing >= 0) {
        moved = a->recent_paths[existing];
        for (int i = existing; i + 1 < a->recent_count; ++i)
            a->recent_paths[i] = a->recent_paths[i + 1];
        a->recent_count--;
    }
    if (a->recent_count == cap) {
        free(a->recent_paths[cap - 1]);
        a->recent_count--;
    }
    for (int i = a->recent_count; i > 0; --i)
        a->recent_paths[i] = a->recent_paths[i - 1];
    a->recent_paths[0] = moved ? moved : strdup(path);
    a->recent_count++;
}

static void recent_save(App* a)
{
    FILE* f = fopen("data/.recent", "wb");
    if (!f) return;
    for (int i = 0; i < a->recent_count; ++i)
        fprintf(f, "%s\n", a->recent_paths[i]);
    fclose(f);
}

static void recent_load(App* a)
{
    FILE* f = fopen("data/.recent", "rb");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (n == 0) continue;
        recent_push(a, line);     /* recent_push handles MRU semantics */
    }
    fclose(f);
}

/* True if `path` looks like a raster image our preview can display. The
 * list mirrors vault.c's has_image_ext so the sidebar and the loader agree
 * on what's an image. */
static bool path_is_image(const char* path)
{
    if (!path) return false;
    size_t n = strlen(path);
    static const char* exts[] = {
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp",
    };
    for (size_t i = 0; i < sizeof exts / sizeof exts[0]; ++i) {
        size_t en = strlen(exts[i]);
        if (n <= en) continue;
        bool match = true;
        for (size_t j = 0; j < en; ++j) {
            char c = path[n - en + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != exts[i][j]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

static int load_note(App* a, const char* path)
{
    /* Image file? Synthesize a tiny markdown buffer whose only line is an
     * image embed so the existing LINE_IMAGE renderer picks it up. The
     * note is force-readonly so accidental keypresses don't corrupt the
     * actual file. */
    if (path_is_image(path)) {
        const char* base = vault_basename(path);
        char body[1024];
        snprintf(body, sizeof body, "# %s\n\n![](%s)\n", base, base);
        buffer_set_text(&a->buf, body, strlen(body));
        a->buf.dirty = false;
        a->viewing_image = true;
        reparse_preview(a);
        free(a->note_path);
        a->note_path      = strdup(path);
        a->scroll_y       = 0;
        a->doc_height_px  = 0;
        a->vault.selected = vault_index_of(&a->vault, path);
        a->edit_mode      = false;     /* preview is the only useful view */
        recent_push(a, path);
        recent_save(a);
        update_window_title(a);
        fprintf(stderr, "downsee: opened image %s\n", path);
        return 0;
    }
    a->viewing_image = false;
    size_t src_len = 0;
    char* src = slurp(path, &src_len);
    if (!src) {
        fprintf(stderr, "load_note: cannot open %s\n", path);
        return -1;
    }
    buffer_set_text(&a->buf, src, src_len);
    free(src);

    reparse_preview(a);

    free(a->note_path);
    a->note_path      = strdup(path);
    a->scroll_y       = 0;
    a->doc_height_px  = 0;
    a->vault.selected = vault_index_of(&a->vault, path);

    recent_push(a, path);
    recent_save(a);

    update_window_title(a);
    fprintf(stderr, "downsee: opened %s (%zu lines)\n", path, a->doc.line_count);
    return 0;
}

/* Forward decl: confirm_discard's event pump calls app_render. */
static void app_render(App* a);
static int SDLCALL resize_event_watch(void* userdata, SDL_Event* e);
static int  settings_persist(App* a);
static int  overlay_list_scrollbar_geom(int box_x, int box_w,
                                        int rows_top, int rows_bot,
                                        int content_h, int scroll,
                                        SDL_Rect* track_out, SDL_Rect* thumb_out);
static void overlay_scrollbar_draw(App* a, const SDL_Rect* track,
                                   const SDL_Rect* thumb, bool dragging);
static bool overlay_scrollbar_handle_click(App* a,
                                           int btn_x, int btn_y,
                                           const SDL_Rect* track,
                                           const SDL_Rect* thumb,
                                           int sb_kind,
                                           int* scroll, int content_h,
                                           int step_px);
static void sb_inner_track(const SDL_Rect* track, SDL_Rect* inner);
static int  scroll_from_thumb_drag(int mouse_y, int inner_y, int inner_h,
                                   int thumb_h, int drag_offset, int max_sc);
static void recent_dirs_push(App* a, const char* dir);
static void recent_dirs_load(App* a);
static int  filesystem_delete(const char* path, int is_dir);
static bool confirm_action(App* a, const char* title, const char* msg,
                           const char* lab0, const char* lab1);

/* Count '\n'-separated message lines (treats empty msg as 1). */
static int confirm_msg_line_count(const App* a)
{
    if (!a->confirm_msg[0]) return 1;
    int n = 1;
    for (const char* p = a->confirm_msg; *p; ++p) if (*p == '\n') n++;
    return n;
}

/* Confirm-modal layout helpers. Buttons: 0 = Discard, 1 = Cancel (default).
 * Vertical layout (top → bottom): pad / title row / gap / msg row(s) /
 * large gap / hint row / small gap / button row / pad. Message can be
 * multi-line — height grows to fit. */
static SDL_Rect confirm_box_rect(const App* a)
{
    int sz_y  = font_line_height(a->font_body);
    int btn_h = sz_y + 16;
    int box_w = 480;
    int msg_h = sz_y * confirm_msg_line_count(a);
    int box_h = 20 + sz_y + 14 + msg_h + 24 + sz_y + 12 + btn_h + 20;
    return (SDL_Rect){ (a->win_w - box_w) / 2,
                       (a->win_h - box_h) / 2,
                       box_w, box_h };
}

static int confirm_btn_y(const App* a)
{
    SDL_Rect box = confirm_box_rect(a);
    int sz_y  = font_line_height(a->font_body);
    int btn_h = sz_y + 16;
    return box.y + box.h - btn_h - 20;
}

/* Width of a confirm-modal button: text + horizontal pad, with a sane
 * minimum so single-letter labels still look like a button. Lab0/lab1
 * size independently so "Choose folder..." doesn't collide with "Skip
 * for now". */
static int confirm_btn_w(const App* a, const char* label)
{
    int pad = 28;
    int min_w = 100;
    int w = font_measure(a->font_body, label, strlen(label)) + pad;
    return w < min_w ? min_w : w;
}

static int confirm_hit_test(const App* a, int mx, int my)
{
    if (!a->confirm_active) return -1;
    SDL_Rect box = confirm_box_rect(a);
    int sz_y  = font_line_height(a->font_body);
    int btn_h = sz_y + 16;
    const char* lab0 = a->confirm_btn0_label[0] ? a->confirm_btn0_label : NULL;
    const char* lab1 = a->confirm_btn1_label[0] ? a->confirm_btn1_label : "Cancel";
    int w1 = confirm_btn_w(a, lab1);
    int w0 = lab0 ? confirm_btn_w(a, lab0) : 0;
    int btn_y = confirm_btn_y(a);
    int b1_x  = box.x + box.w - w1 - 16;
    int b0_x  = b1_x - w0 - 12;
    if (my < btn_y || my >= btn_y + btn_h) return -1;
    if (lab0 && mx >= b0_x && mx < b0_x + w0) return 0;
    if (mx >= b1_x && mx < b1_x + w1) return 1;
    return -1;
}

static void render_confirm_modal(App* a)
{
    if (!a->confirm_active) return;

    overlay_backdrop(a);
    SDL_Rect box = confirm_box_rect(a);
    overlay_card(a, box);

    int sz_y = font_line_height(a->font_body);
    int btn_h = sz_y + 16;

    /* Row anchors (top of each line). Must match the spacing assumed by
     * confirm_box_rect: pad / title / gap14 / msg / gap24 / hint / gap12
     * / btn / pad. font_draw_line wants a baseline, so we add font_ascent
     * per row. Buttons size to their labels so "Choose folder..." style
     * prompts don't clip. */
    int title_top = box.y + 20;
    int msg_top   = title_top + sz_y + 14;
    int btn_y     = confirm_btn_y(a);
    int hint_top  = btn_y - 12 - sz_y;
    const char* _lab0_for_w = a->confirm_btn0_label[0]
                              ? a->confirm_btn0_label : NULL;
    const char* _lab1_for_w = a->confirm_btn1_label[0]
                              ? a->confirm_btn1_label : "Cancel";
    int w1 = confirm_btn_w(a, _lab1_for_w);
    int w0 = _lab0_for_w ? confirm_btn_w(a, _lab0_for_w) : 0;
    int b1_x  = box.x + box.w - w1 - 16;
    int b0_x  = b1_x - w0 - 12;

    /* Title */
    font_draw_line(a->font_body, a->confirm_title, strlen(a->confirm_title),
                   box.x + 20, title_top + font_ascent(a->font_body),
                   a->fg_link);
    /* Message — split on '\n' so the About dialog (and any future
     * multi-paragraph prompt) renders each line. */
    {
        const char* p = a->confirm_msg;
        int ly = msg_top;
        while (*p) {
            const char* nl = strchr(p, '\n');
            size_t n = nl ? (size_t)(nl - p) : strlen(p);
            font_draw_line(a->font_body, p, n,
                           box.x + 20, ly + font_ascent(a->font_body),
                           a->fg);
            ly += sz_y;
            if (!nl) break;
            p = nl + 1;
        }
    }

    /* lab0 empty means "no left button" — used by info_modal for a single
     * OK dialog. Anything non-empty renders. */
    const char* lab0 = a->confirm_btn0_label[0] ? a->confirm_btn0_label : NULL;
    const char* lab1 = a->confirm_btn1_label[0]
                       ? a->confirm_btn1_label : "Cancel";

    /* Btn0 — neutral fill, hover brightens. Pill-shaped. */
    if (lab0) {
        bool hover = (a->confirm_hover == 0);
        SDL_Rect r = { b0_x, btn_y, w0, btn_h };
        SDL_SetRenderDrawColor(a->renderer,
            a->bg_sidebar_hover.r, a->bg_sidebar_hover.g,
            a->bg_sidebar_hover.b, hover ? 255 : 180);
        fill_rrect(a->renderer, r, btn_h / 2);
        int lw = font_measure(a->font_body, lab0, strlen(lab0));
        font_draw_line(a->font_body, lab0, strlen(lab0),
                       b0_x + (w0 - lw) / 2,
                       btn_y + (btn_h - sz_y) / 2 + font_ascent(a->font_body),
                       a->fg);
    }
    /* Btn1 — default action, accent fill. */
    {
        bool hover = (a->confirm_hover == 1);
        SDL_Rect r = { b1_x, btn_y, w1, btn_h };
        SDL_SetRenderDrawColor(a->renderer,
            a->fg_link.r, a->fg_link.g, a->fg_link.b, hover ? 255 : 220);
        fill_rrect(a->renderer, r, btn_h / 2);
        int lw = font_measure(a->font_body, lab1, strlen(lab1));
        int lum = a->fg_link.r * 30 + a->fg_link.g * 59 + a->fg_link.b * 11;
        SDL_Color tc = (lum > 12000) ? (SDL_Color){20, 20, 26, 255}
                                     : (SDL_Color){240, 240, 250, 255};
        font_draw_line(a->font_body, lab1, strlen(lab1),
                       b1_x + (w1 - lw) / 2,
                       btn_y + (btn_h - sz_y) / 2 + font_ascent(a->font_body),
                       tc);
    }

    /* Hint */
    const char* hint = "Enter / Esc cancel  -  Y confirm";
    font_draw_line(a->font_body, hint, strlen(hint),
                   box.x + 20,
                   hint_top + font_ascent(a->font_body),
                   a->fg_muted);
}

/* Synchronous yes/no modal. lab0 is the affirmative action (returns true),
 * lab1 is the safe/cancel (returns false). Both labels are optional: NULL
 * falls back to "Discard" / "Cancel" so legacy callers still read right. */
static bool confirm_action(App* a, const char* title, const char* msg,
                           const char* lab0, const char* lab1)
{
    snprintf(a->confirm_title, sizeof a->confirm_title, "%s", title ? title : "");
    snprintf(a->confirm_msg,   sizeof a->confirm_msg,   "%s", msg   ? msg   : "");
    if (lab0) snprintf(a->confirm_btn0_label, sizeof a->confirm_btn0_label,
                       "%s", lab0);
    else      a->confirm_btn0_label[0] = 0;
    if (lab1) snprintf(a->confirm_btn1_label, sizeof a->confirm_btn1_label,
                       "%s", lab1);
    else      a->confirm_btn1_label[0] = 0;
    a->confirm_active = true;
    a->confirm_choice = -1;
    a->confirm_hover  = 1;     /* default to the safe button */

    while (a->confirm_choice < 0 && a->running) {
        SDL_Event e;
        if (SDL_WaitEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT:
                    a->confirm_choice = 1;
                    break;
                case SDL_MOUSEMOTION:
                    a->confirm_hover = confirm_hit_test(a, e.motion.x, e.motion.y);
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (e.button.button == SDL_BUTTON_LEFT) {
                        int btn = confirm_hit_test(a, e.button.x, e.button.y);
                        if (btn >= 0) a->confirm_choice = btn;
                    }
                    break;
                case SDL_KEYDOWN: {
                    SDL_Keycode k = e.key.keysym.sym;
                    if (k == SDLK_ESCAPE) a->confirm_choice = 1;
                    else if (k == SDLK_RETURN || k == SDLK_KP_ENTER)
                        a->confirm_choice = a->confirm_hover < 0 ? 1 : a->confirm_hover;
                    else if (k == SDLK_y) a->confirm_choice = 0;
                    else if (k == SDLK_n) a->confirm_choice = 1;
                    else if (k == SDLK_TAB || k == SDLK_LEFT || k == SDLK_RIGHT)
                        a->confirm_hover = (a->confirm_hover == 0) ? 1 : 0;
                    break;
                }
            }
        }
        app_render(a);
    }

    a->confirm_active = false;
    a->confirm_btn0_label[0] = 0;
    a->confirm_btn1_label[0] = 0;
    return a->confirm_choice == 0;
}

static bool confirm_discard(App* a)
{
    if (!a->buf.dirty) return true;
    return confirm_action(a, "Unsaved changes",
        "You have unsaved changes. Discard them?", "Discard", "Cancel");
}

/* Single-button info dialog. Pass NULL for lab0 → renders as one OK pill. */
static void info_modal(App* a, const char* title, const char* msg)
{
    (void)confirm_action(a, title, msg, NULL, "OK");
}

/* ----------------------------- text-input modal ------------------------ */

/* Geometry: tall enough to host a 10-row file list under the input. */
static SDL_Rect tinput_box_rect(App* a)
{
    int w = 560;
    int h = 480;
    return (SDL_Rect){ (a->win_w - w) / 2, (a->win_h - h) / 2, w, h };
}

static int tinput_list_row_h(App* a) { return font_line_height(a->font_body) + 6; }
static int tinput_list_top  (App* a)
{
    SDL_Rect box = tinput_box_rect(a);
    int sz_y = font_line_height(a->font_body);
    int input_h = sz_y + 16;
    int in_y    = box.y + 20 + sz_y + 8;
    int dir_y   = in_y + input_h + 8;
    return dir_y + sz_y + 8;
}

static int tinput_list_bot  (App* a)
{
    SDL_Rect box = tinput_box_rect(a);
    int sz_y  = font_line_height(a->font_body);
    int btn_h = sz_y + 16;
    int btn_y = box.y + box.h - btn_h - 16;
    int hint_h = sz_y + 6;
    return btn_y - hint_h - 8;
}

static int tinput_hit_test(App* a, int mx, int my)
{
    if (!a->tinput_active) return -1;
    SDL_Rect box = tinput_box_rect(a);
    int btn_h = font_line_height(a->font_body) + 16;
    int btn_y = box.y + box.h - btn_h - 16;
    /* Match render's label-sized buttons. */
    const char* lab_ok = a->tinput_pick_dir ? "Use this folder" : "Save";
    const char* lab_cn = "Cancel";
    const char* lab_nf = "New Folder";
    int btn_pad = 28, min_w = 120;
    int w_ok = font_measure(a->font_body, lab_ok, strlen(lab_ok)) + btn_pad;
    int w_cn = font_measure(a->font_body, lab_cn, strlen(lab_cn)) + btn_pad;
    int w_nf = font_measure(a->font_body, lab_nf, strlen(lab_nf)) + btn_pad;
    if (w_ok < min_w) w_ok = min_w;
    if (w_cn < min_w) w_cn = min_w;
    int b1_x = box.x + box.w - w_ok - 16;
    int b0_x = b1_x - w_cn - 12;
    int b_nf_x = box.x + 16;     /* leftmost; only present in pick_dir mode */
    if (my >= btn_y && my < btn_y + btn_h) {
        bool nf_visible = a->tinput_pick_dir &&
            strcmp(a->tinput_dir, COMPUTER_SENTINEL) != 0 &&
            a->tinput_dir[0];
        if (nf_visible && mx >= b_nf_x && mx < b_nf_x + w_nf) return 2;
        if (mx >= b0_x && mx < b0_x + w_cn) return 1;
        if (mx >= b1_x && mx < b1_x + w_ok) return 0;
    }
    return -1;
}

/* Map (mx, my) to a file-list row index, or -1 if outside the list area. */
static int tinput_files_row_at(App* a, int mx, int my)
{
    if (!a->tinput_active || a->tinput_files_count == 0) return -1;
    SDL_Rect box = tinput_box_rect(a);
    int top = tinput_list_top(a);
    int bot = tinput_list_bot(a);
    if (mx < box.x + 16 || mx >= box.x + box.w - 16) return -1;
    if (my < top || my >= bot) return -1;
    int rh = tinput_list_row_h(a);
    int row = (my - top + a->tinput_files_scroll) / rh;
    if (row < 0 || row >= a->tinput_files_count) return -1;
    return row;
}

/* Free + null the file list. */
static void tinput_files_clear(App* a)
{
    for (int i = 0; i < a->tinput_files_count; ++i) free(a->tinput_files[i]);
    a->tinput_files_count = 0;
}

/* Find the row index whose basename matches `name`, or -1 if absent.
 * Used to highlight the freshly-created / renamed folder. */
static int tinput_files_find(App* a, const char* name)
{
    if (!name) return -1;
    for (int i = 0; i < a->tinput_files_count; ++i) {
        if (a->tinput_files[i] && strcmp(a->tinput_files[i], name) == 0)
            return i;
    }
    return -1;
}

/* Scroll the file list so `row` is in view (clamps to valid range). */
static void tinput_files_ensure_visible(App* a, int row)
{
    if (row < 0) return;
    int rh = tinput_list_row_h(a);
    int top = tinput_list_top(a);
    int bot = tinput_list_bot(a);
    int row_y = row * rh;
    int max_sc = a->tinput_files_count * rh - (bot - top);
    if (max_sc < 0) max_sc = 0;
    if (row_y < a->tinput_files_scroll) a->tinput_files_scroll = row_y;
    int row_bot = row_y + rh;
    int view_h  = bot - top;
    if (row_bot > a->tinput_files_scroll + view_h)
        a->tinput_files_scroll = row_bot - view_h;
    if (a->tinput_files_scroll > max_sc) a->tinput_files_scroll = max_sc;
    if (a->tinput_files_scroll < 0) a->tinput_files_scroll = 0;
}

/* Reserve room for one more entry in tinput_files / tinput_files_isdir. */
static void tinput_files_reserve(App* a)
{
    if (a->tinput_files_count < a->tinput_files_cap) return;
    a->tinput_files_cap = a->tinput_files_cap ? a->tinput_files_cap * 2 : 32;
    a->tinput_files       = realloc(a->tinput_files,
        a->tinput_files_cap * sizeof(char*));
    a->tinput_files_isdir = realloc(a->tinput_files_isdir,
        a->tinput_files_cap * sizeof(bool));
}

/* True if `dir` is a filesystem root (so we don't show a `..` entry). On
 * Windows that's "C:\", "C:/", or a UNC root. On POSIX it's "/". */
static bool tinput_is_root(const char* dir)
{
    if (!dir || !*dir) return true;
    size_t n = strlen(dir);
#ifdef _WIN32
    if (n == 3 && dir[1] == ':' &&
        (dir[2] == '\\' || dir[2] == '/')) return true;
    if (n == 2 && dir[1] == ':') return true;
#endif
    if (n == 1 && (dir[0] == '/' || dir[0] == '\\')) return true;
    return false;
}

/* "::COMPUTER::" sentinel — when tinput_dir holds this, the file list
 * shows available drives (Windows) or filesystem roots (POSIX) instead
 * of a regular directory listing. Picked when navigating up from the
 * root of a drive, or when the user clears the path bar. */
/* Join `base` + "/" + `leaf`, stripping trailing separators from `base`
 * and leading ones from `leaf` so we never produce paths like "D://X"
 * (the double slash makes opendir / rmdir / rename fail on Windows). */
static void path_join_safe(char* out, size_t cap,
                           const char* base, const char* leaf)
{
    if (!base) base = "";
    if (!leaf) leaf = "";
    size_t bn = strlen(base);
    while (bn > 0 && (base[bn - 1] == '/' || base[bn - 1] == '\\')) bn--;
    while (*leaf == '/' || *leaf == '\\') leaf++;
    if (bn == 0) snprintf(out, cap, "/%s", leaf);
    else         snprintf(out, cap, "%.*s/%s", (int)bn, base, leaf);
}

static bool path_dir_exists(const char* p)
{
    if (!p || !*p) return false;
    struct stat st;
    if (stat(p, &st) != 0) return false;
    return (st.st_mode & S_IFMT) == S_IFDIR;
}

#ifdef _WIN32
static void tinput_emit_drives(App* a)
{
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        char drv[8];
        snprintf(drv, sizeof drv, "%c:/", 'A' + i);
        tinput_files_reserve(a);
        a->tinput_files[a->tinput_files_count]       = strdup(drv);
        a->tinput_files_isdir[a->tinput_files_count] = true;
        a->tinput_files_count++;
    }
}
#else
static void tinput_emit_drives(App* a)
{
    /* POSIX: surface common roots so the user can navigate from `/`,
     * `/home`, `/media`, `/mnt`, `/Volumes` (macOS). */
    static const char* candidates[] = {
        "/", "/home", "/media", "/mnt", "/Volumes",
    };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; ++i) {
        if (!path_dir_exists(candidates[i])) continue;
        tinput_files_reserve(a);
        a->tinput_files[a->tinput_files_count]       = strdup(candidates[i]);
        a->tinput_files_isdir[a->tinput_files_count] = true;
        a->tinput_files_count++;
    }
}
#endif

/* Scan a directory; collect basenames into tinput_files[] with a parallel
 * is-dir flag. Directories sort first (A-Z), then files (A-Z). A `..`
 * synthetic entry is prepended unless the dir is a filesystem root.
 * Special: the COMPUTER_SENTINEL pseudo-dir lists drives (Windows) or
 * common roots (POSIX). */
static void tinput_files_scan(App* a, const char* dir)
{
    tinput_files_clear(a);
    if (!dir || !*dir) return;
    if (strcmp(dir, COMPUTER_SENTINEL) == 0) {
        tinput_emit_drives(a);
        return;
    }
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* ent;
    char path[1024];
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        bool is_dir = false;
#ifdef DT_DIR
        if (ent->d_type == DT_DIR)      is_dir = true;
        else if (ent->d_type == DT_REG) is_dir = false;
        else
#endif
        {
            snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) is_dir = true;
        }
        tinput_files_reserve(a);
        a->tinput_files[a->tinput_files_count]       = strdup(ent->d_name);
        a->tinput_files_isdir[a->tinput_files_count] = is_dir;
        a->tinput_files_count++;
    }
    closedir(d);
    /* Insertion sort: directories first (alphabetical), then files. */
    for (int i = 1; i < a->tinput_files_count; ++i) {
        char* k_name = a->tinput_files[i];
        bool  k_dir  = a->tinput_files_isdir[i];
        int j = i - 1;
        while (j >= 0) {
            bool j_dir = a->tinput_files_isdir[j];
            int cmp;
            if (j_dir != k_dir) cmp = (j_dir ? -1 : 1); /* dir < file */
            else                cmp = strcmp(a->tinput_files[j], k_name);
            if (cmp <= 0) break;
            a->tinput_files[j + 1]       = a->tinput_files[j];
            a->tinput_files_isdir[j + 1] = a->tinput_files_isdir[j];
            j--;
        }
        a->tinput_files[j + 1]       = k_name;
        a->tinput_files_isdir[j + 1] = k_dir;
    }
    /* Prepend a synthetic ".." entry so the user can always navigate up.
     * The COMPUTER view skips this since there's nowhere above it; from
     * any other folder (including a drive root like "C:\") clicking ".."
     * routes through tinput_navigate, which sends drive roots to the
     * COMPUTER view so the user can switch drives. */
    if (strcmp(dir, COMPUTER_SENTINEL) != 0) {
        tinput_files_reserve(a);
        for (int i = a->tinput_files_count; i > 0; --i) {
            a->tinput_files[i]       = a->tinput_files[i - 1];
            a->tinput_files_isdir[i] = a->tinput_files_isdir[i - 1];
        }
        a->tinput_files[0]       = strdup("..");
        a->tinput_files_isdir[0] = true;
        a->tinput_files_count++;
    }
}

/* Path-join helper: join `base` + "/" + `leaf`, then canonicalize trailing
 * separators. If leaf == "..", drop the last segment of base. Writes into
 * `out`, capacity `cap`. */
static void tinput_path_join(const char* base, const char* leaf,
                             char* out, size_t cap)
{
    if (strcmp(leaf, "..") == 0) {
        size_t n = strlen(base);
        while (n > 0 && (base[n - 1] == '/' || base[n - 1] == '\\')) n--;
        while (n > 0 && base[n - 1] != '/' && base[n - 1] != '\\') n--;
        while (n > 1 && (base[n - 1] == '/' || base[n - 1] == '\\')) n--;
        if (n == 0) n = 1; /* preserve leading slash on POSIX */
        if (n >= cap) n = cap - 1;
        memcpy(out, base, n);
        out[n] = 0;
#ifdef _WIN32
        /* On Windows we want "C:" to become "C:\" so opendir works. */
        size_t m = strlen(out);
        if (m == 2 && out[1] == ':' && m + 1 < cap) {
            out[m] = '\\'; out[m + 1] = 0;
        }
#endif
        return;
    }
    size_t bn = strlen(base);
    while (bn > 0 && (base[bn - 1] == '/' || base[bn - 1] == '\\')) bn--;
    if (bn == 0) snprintf(out, cap, "/%s", leaf);
    else         snprintf(out, cap, "%.*s/%s", (int)bn, base, leaf);
}

/* Navigate the file picker to `newdir` (or up via leaf=".."). Updates
 * tinput_dir, resets scroll/hover, re-scans. Goes to the COMPUTER view
 * when navigating up from a drive root, so the user always has a way
 * to switch drives without typing. */
static void tinput_navigate(App* a, const char* leaf)
{
    /* Going up from a drive root or from COMPUTER itself is a no-op for
     * the joiner, so detect that and switch to drive enumeration. */
    if (strcmp(leaf, "..") == 0 &&
        (tinput_is_root(a->tinput_dir) ||
         strcmp(a->tinput_dir, COMPUTER_SENTINEL) == 0))
    {
        snprintf(a->tinput_dir, sizeof a->tinput_dir, COMPUTER_SENTINEL);
        a->tinput_files_scroll = 0;
        a->tinput_files_hover  = -1;
        a->tinput_path_err     = false;
        tinput_files_scan(a, a->tinput_dir);
        if (a->tinput_pick_dir) {
            snprintf(a->tinput_text, sizeof a->tinput_text, "%s",
                     a->tinput_dir);
            a->tinput_len    = (int)strlen(a->tinput_text);
            a->tinput_cursor = a->tinput_len;
        }
        return;
    }
    char next[512];
    /* If we're in COMPUTER view, leaf is a full drive path like "C:/" —
     * use it directly instead of joining onto the sentinel. */
    if (strcmp(a->tinput_dir, COMPUTER_SENTINEL) == 0) {
        snprintf(next, sizeof next, "%s", leaf);
    } else {
        tinput_path_join(a->tinput_dir, leaf, next, sizeof next);
    }
    snprintf(a->tinput_dir, sizeof a->tinput_dir, "%s", next);
    a->tinput_files_scroll = 0;
    a->tinput_files_hover  = -1;
    a->tinput_path_err     = false;
    tinput_files_scan(a, a->tinput_dir);
    if (a->tinput_pick_dir) {
        snprintf(a->tinput_text, sizeof a->tinput_text, "%s", a->tinput_dir);
        a->tinput_len    = (int)strlen(a->tinput_text);
        a->tinput_cursor = a->tinput_len;
    }
}

static int tinput_list_scrollbar_geom(const App* a, const SDL_Rect* list_r,
                                      SDL_Rect* track, SDL_Rect* thumb)
{
    int rh = tinput_list_row_h(a);
    int content_h = a->tinput_files_count * rh;
    return overlay_list_scrollbar_geom(list_r->x, list_r->w,
                                       list_r->y, list_r->y + list_r->h,
                                       content_h, a->tinput_files_scroll,
                                       track, thumb);
}

static void render_tinput_modal(App* a)
{
    if (!a->tinput_active) return;

    overlay_backdrop(a);
    SDL_Rect box = tinput_box_rect(a);
    overlay_card(a, box);

    int sz_y = font_line_height(a->font_body);
    int btn_h = sz_y + 16;
    int btn_w = 120;
    bool pick = a->tinput_pick_dir;

    font_draw_line(a->font_body, a->tinput_title, strlen(a->tinput_title),
                   box.x + 20, box.y + 20 + font_ascent(a->font_body),
                   a->fg_link);

    /* Input field. In save/rename mode this is the filename. In dir-pick
     * mode it doubles as a path bar — typing + Enter navigates; the
     * border turns red when the entered path doesn't exist. */
    int in_y = box.y + 20 + sz_y + 8;
    int in_h = sz_y + 16;
    SDL_Rect in_r = { box.x + 20, in_y, box.w - 40, in_h };
    SDL_SetRenderDrawColor(a->renderer,
        a->bg.r, a->bg.g, a->bg.b, 255);
    fill_rrect(a->renderer, in_r, 6);
    SDL_Color border_c = a->tinput_path_err
        ? (SDL_Color){230, 110, 110, 220}
        : (SDL_Color){a->fg_link.r, a->fg_link.g, a->fg_link.b, 200};
    SDL_SetRenderDrawColor(a->renderer,
        border_c.r, border_c.g, border_c.b, border_c.a);
    draw_rrect(a->renderer, in_r, 6);

    int tx = in_r.x + 10;
    int ty = in_r.y + (in_h - sz_y) / 2 + font_ascent(a->font_body);
    SDL_Rect clip = { in_r.x + 6, in_r.y + 1, in_r.w - 12, in_r.h - 2 };
    SDL_RenderSetClipRect(a->renderer, &clip);
    if (a->tinput_len > 0) {
        font_draw_line(a->font_body, a->tinput_text, a->tinput_len,
                       tx, ty, a->fg);
    } else if (pick) {
        const char* ph = "Type a folder path  -  Enter to go";
        font_draw_line(a->font_body, ph, strlen(ph), tx, ty, a->fg_muted);
    }
    int cw = font_measure(a->font_body, a->tinput_text, a->tinput_cursor);
    SDL_Rect caret = { tx + cw, in_r.y + 4, 2, in_h - 8 };
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_cursor.r, a->fg_cursor.g, a->fg_cursor.b, 255);
    SDL_RenderFillRect(a->renderer, &caret);
    SDL_RenderSetClipRect(a->renderer, NULL);

    /* Reserve room for the hint text above the buttons so the file list
     * stops short of it instead of overlapping. */
    int btn_y_pre = box.y + box.h - btn_h - 16;
    int hint_h    = sz_y + 6;

    /* Currently-shown path label below the input ("in /some/path" or
     * "Computer" when on the drive view). Red when path errored —
     * quotes the path the user actually tried, not the one we fell
     * back to. */
    int dir_y = in_r.y + in_r.h + 8;
    char dir_lab[600];
    if (a->tinput_path_err) {
        snprintf(dir_lab, sizeof dir_lab, "no such folder: %s",
                 a->tinput_err_text[0] ? a->tinput_err_text : a->tinput_dir);
    } else if (strcmp(a->tinput_dir, COMPUTER_SENTINEL) == 0) {
        snprintf(dir_lab, sizeof dir_lab, "in %s",
                 "Computer  (drives)");
    } else if (a->tinput_dir[0]) {
        snprintf(dir_lab, sizeof dir_lab, "in %s", a->tinput_dir);
    } else {
        dir_lab[0] = 0;
    }
    if (dir_lab[0]) {
        SDL_Color lc = a->tinput_path_err
            ? (SDL_Color){230, 110, 110, 220} : a->fg_muted;
        font_draw_line(a->font_body, dir_lab, strlen(dir_lab),
                       box.x + 20, dir_y + font_ascent(a->font_body), lc);
    }

    int list_top = dir_y + sz_y + 8;
    int list_bot = btn_y_pre - hint_h - 8;
    int rh = tinput_list_row_h(a);
    SDL_Rect list_r = { box.x + 16, list_top - 2,
                        box.w - 32, list_bot - list_top + 4 };
    SDL_SetRenderDrawColor(a->renderer,
        a->bg_sidebar.r, a->bg_sidebar.g, a->bg_sidebar.b, 200);
    fill_rrect(a->renderer, list_r, 6);
    SDL_RenderSetClipRect(a->renderer, &list_r);
    int y = list_top - a->tinput_files_scroll;
    /* Bigger icons — the previous font_ascent value rendered the SVG so
     * small that the folder/file shapes looked like dashes. */
    int ic_sz = sz_y - 2;
    for (int i = 0; i < a->tinput_files_count; ++i, y += rh) {
        if (y + rh < list_top || y > list_bot) continue;
        bool is_dir = a->tinput_files_isdir[i];
        bool hov = (i == a->tinput_files_hover);
        bool sel = (i == a->tinput_files_selected);
        bool match = (a->tinput_len > 0 && !is_dir &&
                      strcmp(a->tinput_files[i], a->tinput_text) == 0);
        if (sel) {
            SDL_Rect hr = { list_r.x + 2, y, list_r.w - 4, rh };
            SDL_SetRenderDrawColor(a->renderer,
                a->bg_sidebar_active.r, a->bg_sidebar_active.g,
                a->bg_sidebar_active.b, 255);
            fill_rrect(a->renderer, hr, 4);
            /* Accent left bar so the selection reads even on dim themes. */
            SDL_Rect bar = { list_r.x + 2, y, 3, rh };
            SDL_SetRenderDrawColor(a->renderer,
                a->fg_link.r, a->fg_link.g, a->fg_link.b, 255);
            SDL_RenderFillRect(a->renderer, &bar);
        } else if (hov || match) {
            SDL_Rect hr = { list_r.x + 2, y, list_r.w - 4, rh };
            SDL_SetRenderDrawColor(a->renderer,
                a->bg_sidebar_hover.r, a->bg_sidebar_hover.g,
                a->bg_sidebar_hover.b, hov ? 200 : 130);
            fill_rrect(a->renderer, hr, 4);
        }
        SDL_Color tc = (sel || hov || match) ? a->fg_link : a->fg;
        SDL_Color ic_c = is_dir ? a->fg_link : a->fg_muted;
        IconId ic = is_dir ? ICON_FOLDER : ICON_FILE;
        int ic_y = y + (rh - ic_sz) / 2;
        icon_draw(a->renderer, ic, list_r.x + 12, ic_y, ic_sz, ic_c);
        font_draw_line(a->font_body, a->tinput_files[i],
                       strlen(a->tinput_files[i]),
                       list_r.x + 12 + ic_sz + 8,
                       row_text_baseline(a->font_body, y, rh), tc);
    }
    if (a->tinput_files_count == 0) {
        const char* empty = "(empty directory)";
        font_draw_line(a->font_body, empty, strlen(empty),
                       list_r.x + 12,
                       row_text_baseline(a->font_body, list_top, rh),
                       a->fg_muted);
    }
    SDL_RenderSetClipRect(a->renderer, NULL);

    /* Scrollbar with arrows on the right edge of the file list. */
    SDL_Rect sb_track, sb_thumb;
    if (tinput_list_scrollbar_geom(a, &list_r, &sb_track, &sb_thumb)) {
        overlay_scrollbar_draw(a, &sb_track, &sb_thumb,
                               a->sb_drag == SB_TINPUT);
    }

    /* Inline right-click context menu — Rename + Delete. Rendered last
     * inside the modal so it floats above the file list. */
    if (a->tinput_ctx_active) {
        int rh_ctx = sz_y + 8;
        int mw_ctx = 140;
        int mh_ctx = rh_ctx * 2;
        int mx0 = a->tinput_ctx_x;
        int my0 = a->tinput_ctx_y;
        if (mx0 + mw_ctx > box.x + box.w - 4)
            mx0 = box.x + box.w - 4 - mw_ctx;
        if (my0 + mh_ctx > box.y + box.h - 4)
            my0 = box.y + box.h - 4 - mh_ctx;
        SDL_Rect mr = { mx0, my0, mw_ctx, mh_ctx };
        SDL_SetRenderDrawColor(a->renderer,
            a->bg_sidebar_active.r, a->bg_sidebar_active.g,
            a->bg_sidebar_active.b, 240);
        fill_rrect(a->renderer, mr, 6);
        SDL_SetRenderDrawColor(a->renderer,
            a->fg_link.r, a->fg_link.g, a->fg_link.b, 180);
        draw_rrect(a->renderer, mr, 6);
        const char* labels[2] = { "Rename...", "Delete" };
        SDL_Color  colors[2]  = {
            a->fg, (SDL_Color){230, 110, 110, 255}
        };
        for (int i = 0; i < 2; ++i) {
            font_draw_line(a->font_body, labels[i], strlen(labels[i]),
                           mr.x + 12,
                           row_text_baseline(a->font_body,
                                             mr.y + i * rh_ctx, rh_ctx),
                           colors[i]);
        }
    }

    int btn_y = btn_y_pre;
    /* Buttons size to label so "Use this folder" doesn't overrun. */
    const char* lab_ok = pick ? "Use this folder" : "Save";
    const char* lab_cn = "Cancel";
    const char* lab_nf = "New Folder";
    int btn_pad = 28;
    int w_ok = font_measure(a->font_body, lab_ok, strlen(lab_ok)) + btn_pad;
    int w_cn = font_measure(a->font_body, lab_cn, strlen(lab_cn)) + btn_pad;
    int w_nf = font_measure(a->font_body, lab_nf, strlen(lab_nf)) + btn_pad;
    if (w_ok < btn_w) w_ok = btn_w;
    if (w_cn < btn_w) w_cn = btn_w;
    int b1_x  = box.x + box.w - w_ok - 16;
    int b0_x  = b1_x - w_cn - 12;
    int b_nf_x = box.x + 16;
    /* "New Folder" lives on the left, only in dir-pick mode, and only
     * when we're inside a real folder — the COMPUTER pseudo-dir has no
     * place to put a new folder. */
    bool show_new_folder = pick &&
        strcmp(a->tinput_dir, COMPUTER_SENTINEL) != 0 &&
        a->tinput_dir[0];
    if (show_new_folder) {
        bool hover = (a->tinput_hover == 2);
        SDL_Rect r = { b_nf_x, btn_y, w_nf, btn_h };
        SDL_SetRenderDrawColor(a->renderer,
            a->bg_sidebar_hover.r, a->bg_sidebar_hover.g,
            a->bg_sidebar_hover.b, hover ? 255 : 180);
        fill_rrect(a->renderer, r, btn_h / 2);
        int lw = font_measure(a->font_body, lab_nf, strlen(lab_nf));
        font_draw_line(a->font_body, lab_nf, strlen(lab_nf),
                       b_nf_x + (w_nf - lw) / 2,
                       btn_y + (btn_h - sz_y) / 2 + font_ascent(a->font_body),
                       a->fg);
    }

    /* Cancel button (neutral) */
    {
        bool hover = (a->tinput_hover == 1);
        SDL_Rect r = { b0_x, btn_y, w_cn, btn_h };
        SDL_SetRenderDrawColor(a->renderer,
            a->bg_sidebar_hover.r, a->bg_sidebar_hover.g,
            a->bg_sidebar_hover.b, hover ? 255 : 180);
        fill_rrect(a->renderer, r, btn_h / 2);
        int lw = font_measure(a->font_body, lab_cn, strlen(lab_cn));
        font_draw_line(a->font_body, lab_cn, strlen(lab_cn),
                       b0_x + (w_cn - lw) / 2,
                       btn_y + (btn_h - sz_y) / 2 + font_ascent(a->font_body),
                       a->fg);
    }
    /* OK button (accent) */
    {
        bool hover = (a->tinput_hover == 0);
        SDL_Rect r = { b1_x, btn_y, w_ok, btn_h };
        SDL_SetRenderDrawColor(a->renderer,
            a->fg_link.r, a->fg_link.g, a->fg_link.b, hover ? 255 : 220);
        fill_rrect(a->renderer, r, btn_h / 2);
        int lw = font_measure(a->font_body, lab_ok, strlen(lab_ok));
        int lum = a->fg_link.r * 30 + a->fg_link.g * 59 + a->fg_link.b * 11;
        SDL_Color tc = (lum > 12000) ? (SDL_Color){20, 20, 26, 255}
                                     : (SDL_Color){240, 240, 250, 255};
        font_draw_line(a->font_body, lab_ok, strlen(lab_ok),
                       b1_x + (w_ok - lw) / 2,
                       btn_y + (btn_h - sz_y) / 2 + font_ascent(a->font_body),
                       tc);
    }
    /* Hint */
    const char* hint = pick
        ? "Click a folder to enter  -  ..  to go up  -  Esc cancel"
        : "Enter save  -  Esc cancel";
    font_draw_line(a->font_body, hint, strlen(hint),
                   box.x + 20, btn_y - 8, a->fg_muted);
}

/* ----------------------------- rename popup ---------------------------- */

/* Small overlay dialog shown on top of the file picker. Has its own input
 * field + OK/Cancel buttons so the picker's path bar isn't disturbed
 * while a rename is in flight. Pumps its own SDL events synchronously
 * (see app_rename_popup) — the picker's loop resumes on OK/Cancel. */

static SDL_Rect renpop_box_rect(const App* a)
{
    int w = 440;
    int h = 200;
    return (SDL_Rect){ (a->win_w - w) / 2, (a->win_h - h) / 2, w, h };
}

/* 0 = OK (Rename), 1 = Cancel, -1 = outside any button. */
static int renpop_hit_test(const App* a, int mx, int my)
{
    SDL_Rect box = renpop_box_rect(a);
    int sz_y  = font_line_height(a->font_body);
    int btn_h = sz_y + 16;
    int btn_y = box.y + box.h - btn_h - 16;
    const char* lab_ok = "Rename";
    const char* lab_cn = "Cancel";
    int btn_pad = 28;
    int w_ok = font_measure(a->font_body, lab_ok, strlen(lab_ok)) + btn_pad;
    int w_cn = font_measure(a->font_body, lab_cn, strlen(lab_cn)) + btn_pad;
    if (w_ok < 100) w_ok = 100;
    if (w_cn < 100) w_cn = 100;
    int b1_x = box.x + box.w - w_ok - 16;
    int b0_x = b1_x - w_cn - 12;
    if (mx >= b1_x && mx < b1_x + w_ok &&
        my >= btn_y && my < btn_y + btn_h) return 0;
    if (mx >= b0_x && mx < b0_x + w_cn &&
        my >= btn_y && my < btn_y + btn_h) return 1;
    return -1;
}

static void render_rename_popup(App* a)
{
    if (!a->tinput_renpop_active) return;

    overlay_backdrop(a);
    SDL_Rect box = renpop_box_rect(a);
    overlay_card(a, box);

    int sz_y = font_line_height(a->font_body);

    /* Title */
    const char* title = "Rename";
    font_draw_line(a->font_body, title, strlen(title),
                   box.x + 20, box.y + 20 + font_ascent(a->font_body),
                   a->fg_link);

    /* Subtitle quoting the original name so the user knows exactly
     * what's being renamed. */
    char sub[320];
    snprintf(sub, sizeof sub, "'%s' to:", a->tinput_renpop_old);
    font_draw_line(a->font_body, sub, strlen(sub),
                   box.x + 20,
                   box.y + 20 + sz_y + 6 + font_ascent(a->font_body),
                   a->fg_muted);

    /* Input field */
    int in_y = box.y + 20 + sz_y * 2 + 16;
    int in_h = sz_y + 16;
    SDL_Rect in_r = { box.x + 20, in_y, box.w - 40, in_h };
    SDL_SetRenderDrawColor(a->renderer,
        a->bg.r, a->bg.g, a->bg.b, 255);
    fill_rrect(a->renderer, in_r, 6);
    SDL_Color border_c = a->tinput_renpop_err
        ? (SDL_Color){230, 110, 110, 220}
        : (SDL_Color){a->fg_link.r, a->fg_link.g, a->fg_link.b, 200};
    SDL_SetRenderDrawColor(a->renderer,
        border_c.r, border_c.g, border_c.b, border_c.a);
    draw_rrect(a->renderer, in_r, 6);

    int tx = in_r.x + 10;
    int ty = in_r.y + (in_h - sz_y) / 2 + font_ascent(a->font_body);
    SDL_Rect clip = { in_r.x + 6, in_r.y + 1, in_r.w - 12, in_r.h - 2 };
    SDL_RenderSetClipRect(a->renderer, &clip);
    if (a->tinput_renpop_len > 0) {
        font_draw_line(a->font_body,
                       a->tinput_renpop_text, a->tinput_renpop_len,
                       tx, ty, a->fg);
    }
    int cw = font_measure(a->font_body,
                          a->tinput_renpop_text, a->tinput_renpop_cursor);
    SDL_Rect caret = { tx + cw, in_r.y + 4, 2, in_h - 8 };
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_cursor.r, a->fg_cursor.g, a->fg_cursor.b, 255);
    SDL_RenderFillRect(a->renderer, &caret);
    SDL_RenderSetClipRect(a->renderer, NULL);

    /* Error label below the input, if any. */
    if (a->tinput_renpop_err && a->tinput_renpop_err_text[0]) {
        font_draw_line(a->font_body,
                       a->tinput_renpop_err_text,
                       strlen(a->tinput_renpop_err_text),
                       box.x + 20,
                       in_r.y + in_r.h + 6 + font_ascent(a->font_body),
                       (SDL_Color){230, 110, 110, 255});
    }

    /* Buttons */
    int btn_h = sz_y + 16;
    int btn_y = box.y + box.h - btn_h - 16;
    const char* lab_ok = "Rename";
    const char* lab_cn = "Cancel";
    int btn_pad = 28;
    int w_ok = font_measure(a->font_body, lab_ok, strlen(lab_ok)) + btn_pad;
    int w_cn = font_measure(a->font_body, lab_cn, strlen(lab_cn)) + btn_pad;
    if (w_ok < 100) w_ok = 100;
    if (w_cn < 100) w_cn = 100;
    int b1_x = box.x + box.w - w_ok - 16;
    int b0_x = b1_x - w_cn - 12;

    /* Cancel (neutral) */
    {
        bool hover = (a->tinput_renpop_hover == 1);
        SDL_Rect r = { b0_x, btn_y, w_cn, btn_h };
        SDL_SetRenderDrawColor(a->renderer,
            a->bg_sidebar_hover.r, a->bg_sidebar_hover.g,
            a->bg_sidebar_hover.b, hover ? 255 : 180);
        fill_rrect(a->renderer, r, btn_h / 2);
        int lw = font_measure(a->font_body, lab_cn, strlen(lab_cn));
        font_draw_line(a->font_body, lab_cn, strlen(lab_cn),
                       b0_x + (w_cn - lw) / 2,
                       btn_y + (btn_h - sz_y) / 2 + font_ascent(a->font_body),
                       a->fg);
    }
    /* Rename (accent) */
    {
        bool hover = (a->tinput_renpop_hover == 0);
        SDL_Rect r = { b1_x, btn_y, w_ok, btn_h };
        SDL_SetRenderDrawColor(a->renderer,
            a->fg_link.r, a->fg_link.g, a->fg_link.b, hover ? 255 : 220);
        fill_rrect(a->renderer, r, btn_h / 2);
        int lw = font_measure(a->font_body, lab_ok, strlen(lab_ok));
        int lum = a->fg_link.r * 30 + a->fg_link.g * 59 + a->fg_link.b * 11;
        SDL_Color tc = (lum > 12000) ? (SDL_Color){20, 20, 26, 255}
                                     : (SDL_Color){240, 240, 250, 255};
        font_draw_line(a->font_body, lab_ok, strlen(lab_ok),
                       b1_x + (w_ok - lw) / 2,
                       btn_y + (btn_h - sz_y) / 2 + font_ascent(a->font_body),
                       tc);
    }
}

/* Modal rename popup. Pre-fills the input with `oldname`. Pumps SDL
 * events until OK or Cancel. On OK, copies the new name into `out` and
 * returns true. Validation (non-empty, no path separators) is enforced
 * here — invalid commits flash the red border and stay open. */
static bool app_rename_popup(App* a, const char* oldname,
                             char* out, size_t out_cap)
{
    snprintf(a->tinput_renpop_old, sizeof a->tinput_renpop_old,
             "%s", oldname ? oldname : "");
    snprintf(a->tinput_renpop_text, sizeof a->tinput_renpop_text,
             "%s", oldname ? oldname : "");
    a->tinput_renpop_len    = (int)strlen(a->tinput_renpop_text);
    a->tinput_renpop_cursor = a->tinput_renpop_len;
    a->tinput_renpop_active = true;
    a->tinput_renpop_choice = -1;
    a->tinput_renpop_hover  = 0;
    a->tinput_renpop_err    = false;
    a->tinput_renpop_err_text[0] = 0;

    SDL_StartTextInput();

    while (a->tinput_renpop_choice < 0 && a->running) {
        SDL_Event e;
        if (SDL_WaitEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT:
                    a->tinput_renpop_choice = 1;
                    break;
                case SDL_MOUSEMOTION:
                    a->tinput_renpop_hover =
                        renpop_hit_test(a, e.motion.x, e.motion.y);
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (e.button.button == SDL_BUTTON_LEFT) {
                        int btn = renpop_hit_test(a, e.button.x, e.button.y);
                        if (btn == 1) {
                            a->tinput_renpop_choice = 1;
                        } else if (btn == 0) {
                            /* Same validation as Enter: non-empty + no
                             * path separators. */
                            const char* nm = a->tinput_renpop_text;
                            bool bad = (a->tinput_renpop_len == 0);
                            for (int i = 0; i < a->tinput_renpop_len && !bad;
                                 ++i)
                                if (nm[i] == '/' || nm[i] == '\\') bad = true;
                            if (bad) {
                                a->tinput_renpop_err = true;
                                snprintf(a->tinput_renpop_err_text,
                                         sizeof a->tinput_renpop_err_text,
                                         "name can't be empty or contain '/' or '\\'");
                            } else {
                                a->tinput_renpop_choice = 0;
                            }
                        }
                    }
                    break;
                case SDL_TEXTINPUT: {
                    int ti = (int)strlen(e.text.text);
                    if (a->tinput_renpop_len + ti <
                        (int)sizeof a->tinput_renpop_text - 1)
                    {
                        memmove(
                            a->tinput_renpop_text +
                                a->tinput_renpop_cursor + ti,
                            a->tinput_renpop_text + a->tinput_renpop_cursor,
                            (size_t)(a->tinput_renpop_len -
                                     a->tinput_renpop_cursor + 1));
                        memcpy(
                            a->tinput_renpop_text + a->tinput_renpop_cursor,
                            e.text.text, (size_t)ti);
                        a->tinput_renpop_cursor += ti;
                        a->tinput_renpop_len    += ti;
                    }
                    a->tinput_renpop_err = false;
                    break;
                }
                case SDL_KEYDOWN: {
                    SDL_Keycode k = e.key.keysym.sym;
                    if (k == SDLK_ESCAPE) {
                        a->tinput_renpop_choice = 1; break;
                    }
                    if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                        const char* nm = a->tinput_renpop_text;
                        bool bad = (a->tinput_renpop_len == 0);
                        for (int i = 0; i < a->tinput_renpop_len && !bad; ++i)
                            if (nm[i] == '/' || nm[i] == '\\') bad = true;
                        if (bad) {
                            a->tinput_renpop_err = true;
                            snprintf(a->tinput_renpop_err_text,
                                     sizeof a->tinput_renpop_err_text,
                                     "name can't be empty or contain '/' or '\\'");
                        } else {
                            a->tinput_renpop_choice = 0;
                        }
                        break;
                    }
                    if (k == SDLK_BACKSPACE && a->tinput_renpop_cursor > 0) {
                        memmove(
                            a->tinput_renpop_text +
                                a->tinput_renpop_cursor - 1,
                            a->tinput_renpop_text + a->tinput_renpop_cursor,
                            (size_t)(a->tinput_renpop_len -
                                     a->tinput_renpop_cursor + 1));
                        a->tinput_renpop_cursor--;
                        a->tinput_renpop_len--;
                        a->tinput_renpop_err = false;
                    }
                    if (k == SDLK_DELETE &&
                        a->tinput_renpop_cursor < a->tinput_renpop_len)
                    {
                        memmove(
                            a->tinput_renpop_text + a->tinput_renpop_cursor,
                            a->tinput_renpop_text +
                                a->tinput_renpop_cursor + 1,
                            (size_t)(a->tinput_renpop_len -
                                     a->tinput_renpop_cursor));
                        a->tinput_renpop_len--;
                        a->tinput_renpop_err = false;
                    }
                    if (k == SDLK_LEFT && a->tinput_renpop_cursor > 0)
                        a->tinput_renpop_cursor--;
                    if (k == SDLK_RIGHT &&
                        a->tinput_renpop_cursor < a->tinput_renpop_len)
                        a->tinput_renpop_cursor++;
                    if (k == SDLK_HOME) a->tinput_renpop_cursor = 0;
                    if (k == SDLK_END)
                        a->tinput_renpop_cursor = a->tinput_renpop_len;
                    break;
                }
            }
        }
        app_render(a);
    }

    a->tinput_renpop_active = false;
    if (a->tinput_renpop_choice == 0 && out && out_cap > 0) {
        snprintf(out, out_cap, "%s", a->tinput_renpop_text);
        return true;
    }
    return false;
}

/* Synchronous text-input modal with optional directory listing. If `dir`
 * is non-NULL/non-empty, the modal scans it once on open and shows a
 * scrollable file list below the input — click a row to fill the input.
 * Pumps SDL events until OK or Cancel. Returns true on OK; result is in
 * a->tinput_text. */
static bool app_text_modal(App* a, const char* title, const char* default_text,
                           const char* dir)
{
    snprintf(a->tinput_title, sizeof a->tinput_title, "%s", title ? title : "");
    if (default_text) {
        snprintf(a->tinput_text, sizeof a->tinput_text, "%s", default_text);
    } else {
        a->tinput_text[0] = 0;
    }
    a->tinput_len    = (int)strlen(a->tinput_text);
    a->tinput_cursor = a->tinput_len;
    a->tinput_active = true;
    a->tinput_choice = -1;
    a->tinput_hover  = 0;     /* OK by default */
    a->tinput_files_hover  = -1;
    a->tinput_files_scroll = 0;
    a->tinput_files_selected = -1;
    a->tinput_path_err     = false;
    a->tinput_err_text[0]  = 0;
    a->tinput_ctx_active   = false;
    a->tinput_renpop_active= false;
    a->tinput_renpop_old[0]= 0;
    if (dir && *dir) {
        snprintf(a->tinput_dir, sizeof a->tinput_dir, "%s", dir);
        tinput_files_scan(a, dir);
    } else {
        a->tinput_dir[0] = 0;
        tinput_files_clear(a);
    }

    SDL_StartTextInput();

    while (a->tinput_choice < 0 && a->running) {
        SDL_Event e;
        if (SDL_WaitEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT:
                    a->tinput_choice = 1;
                    break;
                case SDL_MOUSEMOTION: {
                    a->tinput_hover = tinput_hit_test(a, e.motion.x, e.motion.y);
                    int frow = tinput_files_row_at(a, e.motion.x, e.motion.y);
                    a->tinput_files_hover = frow;
                    /* Drag the scrollbar thumb if the user grabbed it. */
                    if (a->sb_drag == SB_TINPUT) {
                        SDL_Rect list_r = {
                            tinput_box_rect(a).x + 16,
                            tinput_list_top(a) - 2,
                            tinput_box_rect(a).w - 32,
                            tinput_list_bot(a) - tinput_list_top(a) + 4
                        };
                        SDL_Rect track, thumb;
                        if (tinput_list_scrollbar_geom(a, &list_r,
                                                       &track, &thumb))
                        {
                            SDL_Rect inner;
                            sb_inner_track(&track, &inner);
                            int rh = tinput_list_row_h(a);
                            int content_h = a->tinput_files_count * rh;
                            int max_sc = content_h - list_r.h;
                            if (max_sc < 0) max_sc = 0;
                            a->tinput_files_scroll = scroll_from_thumb_drag(
                                e.motion.y, inner.y, inner.h, thumb.h,
                                a->sb_drag_offset, max_sc);
                        }
                    }
                    break;
                }
                case SDL_MOUSEBUTTONUP:
                    if (a->sb_drag == SB_TINPUT) a->sb_drag = SB_NONE;
                    break;
                /* Right-click on a row opens an inline context menu (one
                 * option for now: Rename). Anywhere else dismisses an
                 * already-open menu. Handled before the LEFT case so a
                 * left-click on the menu can pick the option. */
                case SDL_USEREVENT:    /* unused — placeholder slot */
                    break;
                case SDL_MOUSEWHEEL: {
                    int rh = tinput_list_row_h(a);
                    a->tinput_files_scroll -= e.wheel.y * rh * 2;
                    if (a->tinput_files_scroll < 0) a->tinput_files_scroll = 0;
                    int content_h = a->tinput_files_count * rh;
                    int visible_h = tinput_list_bot(a) - tinput_list_top(a);
                    int max_sc = content_h - visible_h;
                    if (max_sc < 0) max_sc = 0;
                    if (a->tinput_files_scroll > max_sc)
                        a->tinput_files_scroll = max_sc;
                    break;
                }
                case SDL_MOUSEBUTTONDOWN:
                    /* Right-click on a file row opens an inline ctx menu
                     * with a single Rename option. ".." is excluded. */
                    if (e.button.button == SDL_BUTTON_RIGHT) {
                        int frow = tinput_files_row_at(a, e.button.x, e.button.y);
                        if (frow >= 0 && a->tinput_files[frow] &&
                            strcmp(a->tinput_files[frow], "..") != 0)
                        {
                            a->tinput_ctx_active = true;
                            a->tinput_ctx_row    = frow;
                            a->tinput_ctx_x      = e.button.x;
                            a->tinput_ctx_y      = e.button.y;
                        } else {
                            a->tinput_ctx_active = false;
                        }
                        break;
                    }
                    if (e.button.button == SDL_BUTTON_LEFT) {
                        /* Click on the inline ctx menu? Two rows:
                         *   row 0 = Rename, row 1 = Delete.
                         * Anything else dismisses the menu and falls
                         * through to normal handling. */
                        if (a->tinput_ctx_active) {
                            int rh_ctx = font_line_height(a->font_body) + 8;
                            int mw_ctx = 140;
                            int mh_ctx = rh_ctx * 2;
                            int mx0 = a->tinput_ctx_x;
                            int my0 = a->tinput_ctx_y;
                            bool on_menu =
                                e.button.x >= mx0 && e.button.x < mx0 + mw_ctx &&
                                e.button.y >= my0 && e.button.y < my0 + mh_ctx;
                            int item = on_menu
                                ? (e.button.y - my0) / rh_ctx : -1;
                            int rrow = a->tinput_ctx_row;
                            if (item == 0 && rrow >= 0 &&
                                rrow < a->tinput_files_count)
                            {
                                /* Rename via popup. Closes ctx menu first
                                 * so the popup doesn't render with a stale
                                 * menu still drawn underneath. The popup
                                 * pumps its own events; on OK it returns
                                 * the new name and we perform the rename
                                 * + refresh the listing here. */
                                char old_nm[260];
                                snprintf(old_nm, sizeof old_nm, "%s",
                                         a->tinput_files[rrow]);
                                a->tinput_ctx_active = false;
                                char new_nm[260];
                                if (app_rename_popup(a, old_nm,
                                                     new_nm, sizeof new_nm))
                                {
                                    char src[1024], dst[1024];
                                    path_join_safe(src, sizeof src,
                                                   a->tinput_dir, old_nm);
                                    path_join_safe(dst, sizeof dst,
                                                   a->tinput_dir, new_nm);
                                    if (rename(src, dst) == 0) {
                                        tinput_files_scan(a, a->tinput_dir);
                                        int rrow2 = tinput_files_find(
                                            a, new_nm);
                                        a->tinput_files_selected = rrow2;
                                        tinput_files_ensure_visible(a, rrow2);
                                        a->tinput_path_err = false;
                                        a->tinput_err_text[0] = 0;
                                    } else {
                                        a->tinput_path_err = true;
                                        snprintf(a->tinput_err_text,
                                                 sizeof a->tinput_err_text,
                                                 "rename failed: %s",
                                                 old_nm);
                                    }
                                }
                                break;
                            } else if (item == 1 && rrow >= 0 &&
                                       rrow < a->tinput_files_count)
                            {
                                /* Delete with a confirm prompt. Re-enters
                                 * confirm_action's pump nested inside the
                                 * picker — both modals draw, the inner
                                 * grabs events, original picker resumes
                                 * after the choice. */
                                char victim[1024];
                                path_join_safe(victim, sizeof victim,
                                               a->tinput_dir,
                                               a->tinput_files[rrow]);
                                bool is_dir =
                                    a->tinput_files_isdir[rrow];
                                char prompt[1200];
                                snprintf(prompt, sizeof prompt,
                                    "Delete \"%s\"%s?\nThis cannot be undone.",
                                    a->tinput_files[rrow],
                                    is_dir ? " (folder must be empty)" : "");
                                a->tinput_ctx_active = false;
                                if (confirm_action(a, "Delete", prompt,
                                                   "Delete", "Cancel"))
                                {
                                    if (filesystem_delete(victim, is_dir) == 0) {
                                        tinput_files_scan(a, a->tinput_dir);
                                        a->tinput_files_selected = -1;
                                    } else {
                                        a->tinput_path_err = true;
                                        snprintf(a->tinput_err_text,
                                                 sizeof a->tinput_err_text,
                                                 "delete failed: %s",
                                                 victim);
                                    }
                                }
                                break;
                            }
                            a->tinput_ctx_active = false;
                            if (on_menu) break;
                        }
                        /* Scrollbar takes priority over file rows. */
                        SDL_Rect list_r = {
                            tinput_box_rect(a).x + 16,
                            tinput_list_top(a) - 2,
                            tinput_box_rect(a).w - 32,
                            tinput_list_bot(a) - tinput_list_top(a) + 4
                        };
                        SDL_Rect track, thumb;
                        if (tinput_list_scrollbar_geom(a, &list_r,
                                                       &track, &thumb)
                            && overlay_scrollbar_handle_click(
                                a, e.button.x, e.button.y,
                                &track, &thumb, SB_TINPUT,
                                &a->tinput_files_scroll,
                                a->tinput_files_count * tinput_list_row_h(a),
                                tinput_list_row_h(a)))
                        {
                            break;
                        }
                        int btn_pre = tinput_hit_test(a, e.button.x, e.button.y);
                        /* OK button while in dir-pick mode: if a folder
                         * row is highlighted, that's what the user wants
                         * to commit (Windows Explorer "Open" semantics).
                         * Otherwise commit the current dir, refusing the
                         * COMPUTER sentinel and any non-existent path. */
                        if (btn_pre == 0 && a->tinput_pick_dir) {
                            int sel = a->tinput_files_selected;
                            if (sel >= 0 && sel < a->tinput_files_count &&
                                a->tinput_files_isdir[sel] &&
                                strcmp(a->tinput_files[sel], "..") != 0)
                            {
                                char chosen[1024];
                                path_join_safe(chosen, sizeof chosen,
                                               a->tinput_dir,
                                               a->tinput_files[sel]);
                                snprintf(a->tinput_dir,
                                         sizeof a->tinput_dir,
                                         "%s", chosen);
                            }
                            if (strcmp(a->tinput_dir, COMPUTER_SENTINEL) == 0
                                || !path_dir_exists(a->tinput_dir))
                            {
                                a->tinput_path_err = true;
                                snprintf(a->tinput_err_text,
                                         sizeof a->tinput_err_text,
                                         "%s", a->tinput_dir);
                                break;
                            }
                        }
                        /* "New Folder" button — creates "Downsee Vault" by
                         * default (or the typed name if the path bar holds
                         * a leaf name). On success, navigates into the new
                         * folder AND drops into rename mode so the user
                         * can immediately type a better name. */
                        if (btn_pre == 2 && a->tinput_pick_dir) {
                            if (strcmp(a->tinput_dir, COMPUTER_SENTINEL) == 0
                                || !a->tinput_dir[0])
                            {
                                a->tinput_path_err = true;
                                snprintf(a->tinput_err_text,
                                         sizeof a->tinput_err_text,
                                         "%s", a->tinput_dir);
                                break;
                            }
                            /* Use typed text only if it's a plain leaf
                             * name (no slashes) AND not the same as the
                             * current path. Otherwise default. */
                            const char* nm = "Downsee Vault";
                            bool typed_ok = (a->tinput_len > 0);
                            for (int i = 0; i < a->tinput_len && typed_ok; ++i) {
                                if (a->tinput_text[i] == '/' ||
                                    a->tinput_text[i] == '\\') typed_ok = false;
                            }
                            if (typed_ok &&
                                strcmp(a->tinput_text, a->tinput_dir) != 0)
                                nm = a->tinput_text;
                            /* If the chosen name already exists, suffix
                             * with (2), (3), ... up to (99). The picked
                             * name (whatever number it ends up being) is
                             * what gets selected and renamed afterwards.
                             * Strip the parent's trailing slash (e.g.
                             * "D:/") before joining or we end up with
                             * "D://name" which downstream openers
                             * mishandle. */
                            char unique[300];
                            char npath[1024];
                            int suffix = 1;
                            bool created = false;
                            while (suffix < 100) {
                                if (suffix == 1)
                                    snprintf(unique, sizeof unique, "%s", nm);
                                else
                                    snprintf(unique, sizeof unique,
                                             "%s (%d)", nm, suffix);
                                path_join_safe(npath, sizeof npath,
                                               a->tinput_dir, unique);
#ifdef _WIN32
                                if (CreateDirectoryA(npath, NULL)) {
                                    created = true; break;
                                }
                                if (GetLastError() != ERROR_ALREADY_EXISTS)
                                    break;
#else
                                if (mkdir(npath, 0755) == 0) {
                                    created = true; break;
                                }
                                if (errno != EEXIST) break;
#endif
                                suffix++;
                            }
                            if (!created) {
                                a->tinput_path_err = true;
                                snprintf(a->tinput_err_text,
                                         sizeof a->tinput_err_text,
                                         "%s", npath);
                                break;
                            }
                            /* Stay in the parent dir; refresh the listing
                             * so the new folder appears, then select +
                             * scroll to it. Pop the rename dialog so the
                             * user can immediately type a better name. */
                            tinput_files_scan(a, a->tinput_dir);
                            a->tinput_files_hover  = -1;
                            a->tinput_path_err     = false;
                            int row = tinput_files_find(a, unique);
                            a->tinput_files_selected = row;
                            tinput_files_ensure_visible(a, row);
                            char new_nm[260];
                            if (app_rename_popup(a, unique,
                                                 new_nm, sizeof new_nm))
                            {
                                char src[1024], dst[1024];
                                path_join_safe(src, sizeof src,
                                               a->tinput_dir, unique);
                                path_join_safe(dst, sizeof dst,
                                               a->tinput_dir, new_nm);
                                if (rename(src, dst) == 0) {
                                    tinput_files_scan(a, a->tinput_dir);
                                    int rrow2 = tinput_files_find(a, new_nm);
                                    a->tinput_files_selected = rrow2;
                                    tinput_files_ensure_visible(a, rrow2);
                                } else {
                                    a->tinput_path_err = true;
                                    snprintf(a->tinput_err_text,
                                             sizeof a->tinput_err_text,
                                             "rename failed: %s", unique);
                                }
                            }
                            break;
                        }
                        int frow = tinput_files_row_at(a, e.button.x, e.button.y);
                        if (frow >= 0) {
                            const char* nm = a->tinput_files[frow];
                            bool dbl = (e.button.clicks >= 2);
                            /* `..` is special — single click navigates up
                             * since there's nothing to "select". Other
                             * folders: single click highlights, double
                             * click enters. */
                            if (strcmp(nm, "..") == 0) {
                                tinput_navigate(a, nm);
                            } else if (a->tinput_files_isdir[frow]) {
                                if (dbl) {
                                    tinput_navigate(a, nm);
                                } else {
                                    a->tinput_files_selected = frow;
                                }
                            } else if (a->tinput_pick_dir) {
                                /* Files in dir-pick mode are
                                 * informational; just allow selection. */
                                a->tinput_files_selected = frow;
                            } else {
                                /* Save/rename mode: select + fill input. */
                                a->tinput_files_selected = frow;
                                snprintf(a->tinput_text, sizeof a->tinput_text,
                                         "%s", nm);
                                a->tinput_len = (int)strlen(a->tinput_text);
                                a->tinput_cursor = a->tinput_len;
                            }
                            break;
                        }
                        int btn = tinput_hit_test(a, e.button.x, e.button.y);
                        if (btn >= 0) a->tinput_choice = btn;
                    }
                    break;
                case SDL_TEXTINPUT: {
                    int ti = (int)strlen(e.text.text);
                    if (a->tinput_len + ti < (int)sizeof(a->tinput_text) - 1) {
                        memmove(a->tinput_text + a->tinput_cursor + ti,
                                a->tinput_text + a->tinput_cursor,
                                (size_t)(a->tinput_len - a->tinput_cursor + 1));
                        memcpy(a->tinput_text + a->tinput_cursor, e.text.text,
                               (size_t)ti);
                        a->tinput_cursor += ti;
                        a->tinput_len    += ti;
                    }
                    /* Reset error state on any keystroke so the red border
                     * disappears the moment the user starts fixing the path. */
                    a->tinput_path_err = false;
                    break;
                }
                case SDL_KEYDOWN: {
                    SDL_Keycode k = e.key.keysym.sym;
                    if (k == SDLK_ESCAPE) { a->tinput_choice = 1; break; }
                    if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                        if (a->tinput_pick_dir) {
                            /* If the typed path differs from the current
                             * dir, treat Enter as a navigate. Existing
                             * folder → switch view; missing → red error.
                             * If they match, treat as commit but refuse
                             * the COMPUTER sentinel / non-existent paths. */
                            if (a->tinput_len > 0 &&
                                strcmp(a->tinput_text, a->tinput_dir) != 0)
                            {
                                if (path_dir_exists(a->tinput_text)) {
                                    snprintf(a->tinput_dir,
                                             sizeof a->tinput_dir,
                                             "%s", a->tinput_text);
                                    a->tinput_files_scroll = 0;
                                    a->tinput_files_hover  = -1;
                                    a->tinput_path_err     = false;
                                    a->tinput_err_text[0]  = 0;
                                    tinput_files_scan(a, a->tinput_dir);
                                } else {
                                    a->tinput_path_err = true;
                                    snprintf(a->tinput_err_text,
                                             sizeof a->tinput_err_text,
                                             "%s", a->tinput_text);
                                }
                                break;
                            }
                            if (strcmp(a->tinput_dir, COMPUTER_SENTINEL) == 0
                                || !path_dir_exists(a->tinput_dir))
                            {
                                a->tinput_path_err = true;
                                break;
                            }
                        }
                        a->tinput_choice = 0; break;
                    }
                    if (k == SDLK_BACKSPACE && a->tinput_cursor > 0) {
                        memmove(a->tinput_text + a->tinput_cursor - 1,
                                a->tinput_text + a->tinput_cursor,
                                (size_t)(a->tinput_len - a->tinput_cursor + 1));
                        a->tinput_cursor--;
                        a->tinput_len--;
                    }
                    if (k == SDLK_DELETE && a->tinput_cursor < a->tinput_len) {
                        memmove(a->tinput_text + a->tinput_cursor,
                                a->tinput_text + a->tinput_cursor + 1,
                                (size_t)(a->tinput_len - a->tinput_cursor));
                        a->tinput_len--;
                    }
                    if (k == SDLK_LEFT  && a->tinput_cursor > 0) a->tinput_cursor--;
                    if (k == SDLK_RIGHT && a->tinput_cursor < a->tinput_len)
                        a->tinput_cursor++;
                    if (k == SDLK_HOME) a->tinput_cursor = 0;
                    if (k == SDLK_END)  a->tinput_cursor = a->tinput_len;
                    break;
                }
            }
        }
        app_render(a);
    }
    SDL_StopTextInput();
    a->tinput_active = false;
    a->tinput_pick_dir = false;
    tinput_files_clear(a);
    return a->tinput_choice == 0;
}

/* In-app folder picker. Reuses the text-modal infrastructure but in
 * pick-dir mode (no input field, "Use this folder" button). On OK,
 * writes the chosen path into `out` and returns true. */
static bool app_dir_modal(App* a, const char* title, const char* start_dir,
                          char* out, size_t out_cap)
{
    a->tinput_pick_dir = true;
    /* When start_dir is empty/NULL, pick a sensible home directory so
     * the user isn't dropped into the exe folder. */
    char fallback[512];
    if (!start_dir || !*start_dir) {
#ifdef _WIN32
        const char* up = getenv("USERPROFILE");
        snprintf(fallback, sizeof fallback, "%s", up ? up : "C:/");
#else
        const char* up = getenv("HOME");
        snprintf(fallback, sizeof fallback, "%s", up ? up : "/");
#endif
        for (char* p = fallback; *p; ++p) if (*p == '\\') *p = '/';
        start_dir = fallback;
    }
    bool ok = app_text_modal(a, title, NULL, start_dir);
    if (ok && out && out_cap > 0)
        snprintf(out, out_cap, "%s", a->tinput_dir);
    return ok;
}

static int app_init(App* a, const char* note_path_arg)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    /* Bilinear scaling for any texture that gets stretched (rare for us
     * since glyphs are blitted 1:1, but cheap insurance). */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    /* Per-DPI: respect the Windows scaling factor so the window is not
     * blurry on hi-DPI monitors. SDL2 doesn't auto-do this on Windows. */
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "0");

    /* Pin cwd to the executable's directory so every relative path
     * (settings.lua next to the exe, plugins/ next to the exe,
     * relative vault_path) resolves consistently no matter how the
     * binary was launched (Explorer shortcut, file association,
     * terminal from elsewhere). Without this, settings_persist writes
     * to the launcher's cwd and the saved theme silently disappears
     * at next launch. */
    {
        char* base = SDL_GetBasePath();
        if (base) {
#if defined(_WIN32)
            SetCurrentDirectoryA(base);
#else
            (void)chdir(base);
#endif
            fprintf(stderr, "downsee: cwd pinned to %s\n", base);
            SDL_free(base);
        }
    }

    a->lua = lua_host_create();
    lua_host_setup_api(a->lua);
    lua_host_on_notify(on_lua_notify, a);
    lua_host_on_dialog(on_lua_dialog, a);

    /* Settings live in ONE file next to the exe: settings.lua. On first
     * run (file missing) we write a defaults stub WITHOUT vault_path —
     * the user picks the vault below and we save it back. Plugins live
     * in a `plugins/` folder next to the exe by default; the user can
     * point elsewhere via plugin_path. */
    bool first_run = (lua_host_load_config(a->lua, "settings.lua") != 0);
    if (first_run) {
        FILE* f = fopen("settings.lua", "wb");
        if (f) {
            fprintf(f,
                "-- Downsee settings (auto-generated on first run).\n"
                "-- Edit by hand or via Settings (Ctrl+,) — values written\n"
                "-- by the settings page will overwrite this file.\n"
                "return {\n"
                "    -- vault_path is filled in after you pick a folder.\n"
                "    -- vault_path = \"C:/Users/me/Notes\",\n"
                "\n"
                "    -- Plugins folder, defaults to 'plugins' next to exe.\n"
                "    plugin_path  = \"plugins\",\n"
                "\n"
                "    window_w     = 1100,\n"
                "    window_h     = 720,\n"
                "    sidebar_open = true,\n"
                "    sidebar_width= 240,\n"
                "\n"
                "    font_path    = \"C:/Windows/Fonts/consola.ttf\",\n"
                "    font_size    = 16,\n"
                "    theme        = \"Editorial Dark\",\n"
                "}\n");
            fclose(f);
            fprintf(stderr, "downsee: first run -- wrote settings.lua\n");
            (void)lua_host_load_config(a->lua, "settings.lua");
        }
    }
    user_kbinds_load_from_cfg(a->lua);
    recent_dirs_load(a);

    {
        const char* pdir = lua_host_cfg_string(a->lua, "plugin_path",
                                               "plugins");
        int n = lua_host_load_plugins(a->lua, pdir);
        if (n > 0) fprintf(stderr, "downsee: loaded %d plugin(s) from %s\n",
                           n, pdir);
    }

    int win_w   = (int)lua_host_cfg_number(a->lua, "window_w", 1100);
    int win_h   = (int)lua_host_cfg_number(a->lua, "window_h",  720);
    int sz_body = (int)lua_host_cfg_number(a->lua, "font_size",     16);
    int sz_h1   = (int)lua_host_cfg_number(a->lua, "font_size_h1",  28);
    int sz_h2   = (int)lua_host_cfg_number(a->lua, "font_size_h2",  22);
    int sz_h3   = (int)lua_host_cfg_number(a->lua, "font_size_h3",  18);

    const char* font_path = lua_host_cfg_string(
        a->lua, "font_path", "C:/Windows/Fonts/consola.ttf");
    const char* font_mono = lua_host_cfg_string(
        a->lua, "font_path_mono", font_path);
    /* Cache the live font config on App so the settings page can mutate it. */
    snprintf(a->cfg_font_path,      sizeof a->cfg_font_path,      "%s", font_path);
    snprintf(a->cfg_font_path_mono, sizeof a->cfg_font_path_mono, "%s", font_mono);
    a->cfg_font_size    = sz_body;
    a->cfg_font_size_h1 = sz_h1;
    a->cfg_font_size_h2 = sz_h2;
    a->cfg_font_size_h3 = sz_h3;
    a->cfg_line_spacing = (int)lua_host_cfg_number(a->lua, "line_spacing", 0);
    if (a->cfg_line_spacing < 0)  a->cfg_line_spacing = 0;
    if (a->cfg_line_spacing > 24) a->cfg_line_spacing = 24;
    a->cfg_line_endings = (int)lua_host_cfg_number(a->lua, "line_endings", 0);
    if (a->cfg_line_endings < 0 || a->cfg_line_endings > 2)
        a->cfg_line_endings = 0;
    font_choices_init();
    a->settings_font_idx = font_choice_find(a->cfg_font_path);
    if (a->settings_font_idx < 0) a->settings_font_idx = 0;
    /* Empty default — first-run check below uses NULL/empty to know
     * we need to prompt the user for a vault folder. We also scrub
     * any sentinel / non-directory value that may have been written
     * to settings.lua by an older buggy build so we don't try to
     * scan "::COMPUTER::" or a deleted folder forever. */
    const char* vault_path = lua_host_cfg_string(a->lua, "vault_path", "");
    if (vault_path && (strcmp(vault_path, "::COMPUTER::") == 0)) {
        vault_path = "";
    }

    a->sidebar_open  = lua_host_cfg_number(a->lua, "sidebar_open",  1) != 0;
    a->sidebar_w     = (int)lua_host_cfg_number(a->lua, "sidebar_width", 240);
    a->outline_pinned   = lua_host_cfg_number(a->lua, "outline_pinned", 0) != 0;
    a->outline_panel_w  = (int)lua_host_cfg_number(a->lua, "outline_panel_width", 240);
    a->sidebar_hover    = -1;
    a->dnd_source_idx   = -1;
    a->dnd_drop_target  = -2;

    /* Theme: if init.lua / overrides set `theme = "Name"`, apply that
     * preset first, then let any explicit `color_*` keys override single
     * fields on top. Defaults to Default Dark. */
    {
        const char* tname = lua_host_cfg_string(a->lua, "theme", "Default Dark");
        int tidx = theme_find(tname);
        if (tidx < 0) tidx = 0;
        a->settings_theme_idx = tidx;
        theme_apply(a, tidx);
    }
    a->bg                = color_from_cfg(a->lua, "color_bg",             a->bg.r,                a->bg.g,                a->bg.b,                a->bg.a);
    a->fg                = color_from_cfg(a->lua, "color_fg",             a->fg.r,                a->fg.g,                a->fg.b,                a->fg.a);
    a->fg_heading        = color_from_cfg(a->lua, "color_heading",        a->fg_heading.r,        a->fg_heading.g,        a->fg_heading.b,        a->fg_heading.a);
    a->fg_quote          = color_from_cfg(a->lua, "color_quote",          a->fg_quote.r,          a->fg_quote.g,          a->fg_quote.b,          a->fg_quote.a);
    a->fg_link           = color_from_cfg(a->lua, "color_link",           a->fg_link.r,           a->fg_link.g,           a->fg_link.b,           a->fg_link.a);
    a->bg_code           = color_from_cfg(a->lua, "color_code_bg",        a->bg_code.r,           a->bg_code.g,           a->bg_code.b,           a->bg_code.a);
    a->fg_muted          = color_from_cfg(a->lua, "color_muted",          a->fg_muted.r,          a->fg_muted.g,          a->fg_muted.b,          a->fg_muted.a);
    a->bg_sidebar        = color_from_cfg(a->lua, "color_sidebar_bg",     a->bg_sidebar.r,        a->bg_sidebar.g,        a->bg_sidebar.b,        a->bg_sidebar.a);
    a->bg_sidebar_hover  = color_from_cfg(a->lua, "color_sidebar_hover",  a->bg_sidebar_hover.r,  a->bg_sidebar_hover.g,  a->bg_sidebar_hover.b,  a->bg_sidebar_hover.a);
    a->bg_sidebar_active = color_from_cfg(a->lua, "color_sidebar_active", a->bg_sidebar_active.r, a->bg_sidebar_active.g, a->bg_sidebar_active.b, a->bg_sidebar_active.a);
    a->bg_status         = color_from_cfg(a->lua, "color_status_bg",      a->bg_status.r,         a->bg_status.g,         a->bg_status.b,         a->bg_status.a);
    a->fg_status         = color_from_cfg(a->lua, "color_status_fg",      a->fg_status.r,         a->fg_status.g,         a->fg_status.b,         a->fg_status.a);
    a->bg_selection      = color_from_cfg(a->lua, "color_selection",      a->bg_selection.r,      a->bg_selection.g,      a->bg_selection.b,      a->bg_selection.a);
    a->fg_cursor         = color_from_cfg(a->lua, "color_cursor",         a->fg_cursor.r,         a->fg_cursor.g,         a->fg_cursor.b,         a->fg_cursor.a);

    a->win_w = win_w;
    a->win_h = win_h;

    /* Best-quality texture scaling: SVG icons rasterize at 3x oversample
     * then bilinear-downscale to render size — without this hint the
     * downscale uses nearest-neighbor and the icons look chunky. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");

    /* Custom decorations: borderless + resizable, with our own title bar
     * (drag, min/max/close, aero snap via SDL_HitTest below). */
    a->window = SDL_CreateWindow("Downsee " DOWNSEE_VERSION,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
        SDL_WINDOW_BORDERLESS);
    if (!a->window) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return -1; }

#if defined(_WIN32)
    /* Win32 magic for "borderless with aero snap":
     *  1. Add WS_THICKFRAME back (BORDERLESS strips it; without it Windows
     *     refuses aero snap, Win+arrow shortcuts, drag-to-edge snap).
     *  2. Subclass WindowProc to intercept:
     *      - WM_NCCALCSIZE   → return 0, keeps client area = full window
     *                          (otherwise WS_THICKFRAME carves out borders)
     *      - WM_GETMINMAXINFO → constrain maximize size to monitor work
     *                          area so the maximized window doesn't cover
     *                          the taskbar.
     *  3. SetWindowPos with SWP_FRAMECHANGED to push the new style live. */
    {
        SDL_SysWMinfo wmi;
        SDL_VERSION(&wmi.version);
        if (SDL_GetWindowWMInfo(a->window, &wmi)) {
            HWND hwnd = wmi.info.win.window;
            LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
            style |= WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX;
            SetWindowLongPtr(hwnd, GWL_STYLE, style);
            g_app_for_wndproc = a;
            g_orig_wndproc = (WNDPROC)SetWindowLongPtr(
                hwnd, GWLP_WNDPROC, (LONG_PTR)downsee_wndproc);
            SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                SWP_NOZORDER     | SWP_NOACTIVATE);
        }
    }
#endif

    a->renderer = SDL_CreateRenderer(a->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!a->renderer) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return -1; }
    SDL_SetRenderDrawBlendMode(a->renderer, SDL_BLENDMODE_BLEND);
    /* Event watch that lets the resize badge actually render while the
     * user drags an edge — on Windows the main loop is blocked inside
     * Defwindowproc during the drag, but our watch fires synchronously
     * whenever SDL pushes a SIZE_CHANGED into the queue. */
    SDL_AddEventWatch(resize_event_watch, a);
    /* Sync win_w/win_h with the window's real size — borderless+highdpi can
     * give us something different than the requested size at create time. */
    SDL_GetWindowSize(a->window, &a->win_w, &a->win_h);
    fprintf(stderr, "downsee: window size %dx%d\n", a->win_w, a->win_h);

    if (app_reload_fonts(a) != 0) return -1;
    if (icons_init(a->renderer) != 0)
        fprintf(stderr, "icons_init failed; chrome will fall back to nothing\n");
    a->menu_open = -1;
    a->tb_btn_hover = -1;
    a->menu_hover   = -1;
    /* Register hit-test so Windows knows what's drag area vs resize edges
     * vs normal content. SDL_HITTEST_DRAGGABLE → HTCAPTION → aero snap and
     * Win+arrow shortcuts work for free. */
    SDL_SetWindowHitTest(a->window, window_hit_test_cb, a);

    buffer_init(&a->buf);
    a->imgcache = image_cache_create(a->renderer);

    vault_init(&a->vault);
    int n = 0;
    bool vault_missing = false;
    if (vault_path && vault_path[0]) {
        /* If the saved vault doesn't exist (deleted, drive unplugged,
         * settings.lua got corrupted), don't silently scan an empty
         * vault — flag it so we re-prompt the user below. */
        bool is_abs = (vault_path[0] == '/' || vault_path[0] == '\\' ||
                       (strlen(vault_path) >= 2 && vault_path[1] == ':'));
        if (!path_dir_exists(vault_path)) {
            if (is_abs) {
                fprintf(stderr,
                    "downsee: saved vault '%s' is gone\n", vault_path);
                vault_missing = true;
            } else {
                /* Relative path — try resolving against the exe's base
                 * dir before giving up. Launching by full path from a
                 * different cwd is the common cause of a relative miss. */
                char* base = SDL_GetBasePath();
                bool resolved_ok = false;
                if (base) {
                    char resolved[1024];
                    snprintf(resolved, sizeof resolved, "%s%s",
                             base, vault_path);
                    if (path_dir_exists(resolved)) {
                        n = vault_scan(&a->vault, resolved);
                        fprintf(stderr,
                            "downsee: vault '%s' (%d items, base-relative)\n",
                            resolved, n);
                        resolved_ok = true;
                    }
                    SDL_free(base);
                }
                if (!resolved_ok) {
                    fprintf(stderr,
                        "downsee: relative vault '%s' not found\n", vault_path);
                    vault_missing = true;
                }
            }
        } else {
            n = vault_scan(&a->vault, vault_path);
            fprintf(stderr, "downsee: vault '%s' (%d items)\n", vault_path, n);
        }
    }

    /* First-run / missing-vault flow. settings.lua had no vault_path,
     * OR the saved one no longer exists on disk (deleted, drive
     * unplugged, etc.) — either way we re-prompt and persist. */
    if (!vault_path || !vault_path[0] || vault_missing) {
        /* Immediately wipe the bad vault_path from settings.lua so the
         * corruption can't reappear on next launch. vault.dir is still
         * NULL/empty here (vault_init sets it that way and we never
         * scanned), so settings_persist will write the file WITHOUT a
         * vault_path line — the rest of the user's config (theme,
         * fonts, recents) is preserved. */
        if (vault_missing) settings_persist(a);
        a->running = true;     /* confirm_action's pump checks this */
        const char* title = vault_missing
            ? "Vault folder not found" : "Welcome to Downsee";
        char msg[400];
        if (vault_missing) {
            /* Truncate from the LEFT with "..." so absurdly mangled
             * paths (a previous build double-prefixed; you're seeing
             * that legacy here) don't overflow the dialog. */
            const char* p = vault_path ? vault_path : "";
            char shown[110];
            size_t pn = strlen(p);
            if (pn > sizeof shown - 1) {
                snprintf(shown, sizeof shown, "...%s",
                         p + (pn - (sizeof shown - 5)));
            } else {
                snprintf(shown, sizeof shown, "%s", p);
            }
            snprintf(msg, sizeof msg,
                "The saved vault folder no longer exists:\n%s\n"
                "Pick a new one to continue.",
                shown);
        } else {
            snprintf(msg, sizeof msg,
                "Pick a folder where your markdown notes live.\n"
                "You can change this later from File > Open Dir...");
        }
        bool pick = confirm_action(a, title, msg,
            "Choose folder...", "Skip for now");
        if (pick) {
            char dir[1024];
            if (app_dir_modal(a, "Choose vault folder", NULL,
                              dir, sizeof dir))
            {
                n = vault_scan(&a->vault, dir);
                fprintf(stderr, "downsee: vault chosen '%s' (%d items)\n",
                        dir, n);
                recent_dirs_push(a, dir);
                settings_persist(a);
            }
        }
    } else if (vault_path && vault_path[0]) {
        /* Successful boot with a saved vault path — record it as recent
         * even on first encounter so the user can quickly switch back
         * after sampling other folders. */
        recent_dirs_push(a, a->vault.dir ? a->vault.dir : vault_path);
    }

    /* Restore the recent files list before any load_note call so we can show
     * the user's most-recent docs even if the start path is the default. */
    recent_load(a);

    const char* start = note_path_arg;
    if (!start && a->vault.count > 0) start = a->vault.items[0].path;
    if (!start) start = "data/sample.md";

    if (load_note(a, start) != 0) {
        /* Fallback chain so a bad CLI arg or missing file never kills the
         * window. Try vault[0], then sample.md, then a welcome doc. */
        bool ok = false;
        if (a->vault.count > 0 && load_note(a, a->vault.items[0].path) == 0)
            ok = true;
        else if (load_note(a, "data/sample.md") == 0)
            ok = true;
        if (!ok) {
            const char* welcome =
                "# Welcome to Downsee\n\n"
                "No note loaded. Try one of these:\n\n"
                "- Press **Ctrl+O** to open a file\n"
                "- Click an item in the sidebar (Ctrl+B to toggle)\n"
                "- Press **Ctrl+E** to switch to edit mode\n";
            free(a->note_path);
            a->note_path = strdup("(welcome)");
            buffer_set_text(&a->buf, welcome, strlen(welcome));
            reparse_preview(a);
            update_window_title(a);
        }
    }

    a->edit_mode = lua_host_cfg_number(a->lua, "start_in_edit_mode", 0) != 0;
    /* Optional: open the settings / keybindings page on launch. Useful for
     * first-run UX and as a debug aid since SDL doesn't always receive
     * synthesized keystrokes from external automation. */
    if (lua_host_cfg_number(a->lua, "start_with_settings", 0) != 0)
        a->settings_active = true;
    if (lua_host_cfg_number(a->lua, "start_with_keybindings", 0) != 0)
        a->keybind_active = true;
    if (a->edit_mode) SDL_StartTextInput();
    a->preview_sel_start = -1;
    a->cursor_resize = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
    a->cur_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    a->cur_ns    = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENS);
    a->cur_we    = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
    a->cur_nwse  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENWSE);
    a->cur_nesw  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENESW);
    a->cur_kind  = 0;

    /* Restore folder-collapse state and saved sidebar width from a sidecar
     * state file in the vault. Lines starting with `@` are app-state
     * directives (currently `@sidebar_w=240`); other lines are paths. */
    {
        char st[1024];
        snprintf(st, sizeof st, "%s/.downsee.state", vault_path);
        FILE* fp = fopen(st, "rb");
        if (fp) {
            char line[1024];
            while (fgets(line, sizeof line, fp)) {
                size_t n = strlen(line);
                while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r'))
                    line[--n] = 0;
                if (n == 0) continue;
                if (line[0] == '@') {
                    if (strncmp(line, "@sidebar_w=", 11) == 0) {
                        int w = atoi(line + 11);
                        if (w >= 120 && w <= win_w / 2) a->sidebar_w = w;
                    }
                    continue;
                }
                for (size_t i = 0; i < a->vault.count; ++i) {
                    if (a->vault.items[i].is_dir &&
                        strcmp(a->vault.items[i].path, line) == 0) {
                        a->vault.items[i].collapsed = 1;
                        break;
                    }
                }
            }
            fclose(fp);
        }
    }

    a->running   = true;
    return 0;
}

static void save_collapse_state(App* a)
{
    if (!a->vault.dir) return;
    char st[1024];
    snprintf(st, sizeof st, "%s/.downsee.state", a->vault.dir);
    FILE* fp = fopen(st, "wb");
    if (!fp) return;
    fprintf(fp, "@sidebar_w=%d\n", a->sidebar_w);
    for (size_t i = 0; i < a->vault.count; ++i) {
        if (a->vault.items[i].is_dir && a->vault.items[i].collapsed)
            fprintf(fp, "%s\n", a->vault.items[i].path);
    }
    fclose(fp);
}

/* Forward decl — persist_vault_path is a tiny wrapper around the
 * full settings_persist defined later in the file. */
static void persist_vault_path(App* a);

static void app_shutdown(App* a)
{
    /* Persist live settings (theme, fonts, sizes, sidebar width, colors,
     * keybindings) so the user's last selections survive a normal close,
     * not just an explicit settings_close. */
    settings_persist(a);
    save_collapse_state(a);
    icons_shutdown();
    md_doc_free(&a->doc);
    buffer_free(&a->buf);
    free(a->note_path);
    free(a->notification_msg);
    free(a->search_matches);
    free(a->search_match_lens);
    free(a->vsearch_hits);
    free(a->outline_entries);
    free(a->backlinks_hits);
    free(a->tags_entries);
    free(a->tpl_entries);
    for (int i = 0; i < a->recent_count; ++i) free(a->recent_paths[i]);
    free(a->switcher_matches);
    free(a->cmdp_matches);
    free(a->cmdp_entries);
    free(a->plugins_rows);
    free(a->wc_matches);
    free(a->hits);
    free(a->sidebar_visible);
    free(a->preview_rows);
    if (a->cursor_resize) SDL_FreeCursor(a->cursor_resize);
    if (a->cur_arrow) SDL_FreeCursor(a->cur_arrow);
    if (a->cur_ns)    SDL_FreeCursor(a->cur_ns);
    if (a->cur_we)    SDL_FreeCursor(a->cur_we);
    if (a->cur_nwse)  SDL_FreeCursor(a->cur_nwse);
    if (a->cur_nesw)  SDL_FreeCursor(a->cur_nesw);
    vault_free(&a->vault);
    if (a->imgcache) image_cache_destroy(a->imgcache);
    if (a->font_body)             font_destroy(a->font_body);
    if (a->font_body_bold)        font_destroy(a->font_body_bold);
    if (a->font_body_italic)      font_destroy(a->font_body_italic);
    if (a->font_body_bold_italic) font_destroy(a->font_body_bold_italic);
    if (a->font_h1)               font_destroy(a->font_h1);
    if (a->font_h2)               font_destroy(a->font_h2);
    if (a->font_h3)               font_destroy(a->font_h3);
    if (a->font_code)             font_destroy(a->font_code);
    if (a->font_code_bold)        font_destroy(a->font_code_bold);
    if (a->font_code_italic)      font_destroy(a->font_code_italic);
    if (a->font_code_bold_italic) font_destroy(a->font_code_bold_italic);
    if (a->lua)                   lua_host_destroy(a->lua);
    if (a->renderer)              SDL_DestroyRenderer(a->renderer);
    if (a->window)                SDL_DestroyWindow(a->window);
    SDL_Quit();
}

/* ----------------------------- preview render --------------------------- */

static Font* pick_font(const App* a, LineKind kind, unsigned char style)
{
    switch (kind) {
        case LINE_H1: return a->font_h1;
        case LINE_H2: return a->font_h2;
        case LINE_H3: case LINE_H4: case LINE_H5: case LINE_H6: return a->font_h3;
        case LINE_CODE: return a->font_code;
        default: break;
    }
    if (style & STYLE_CODE) return a->font_code;
    if ((style & STYLE_BOLD) && (style & STYLE_ITALIC)) return a->font_body_bold_italic;
    if (style & STYLE_BOLD)   return a->font_body_bold;
    if (style & STYLE_ITALIC) return a->font_body_italic;
    return a->font_body;
}

static SDL_Color pick_color(const App* a, LineKind kind, unsigned char style)
{
    if (kind >= LINE_H1 && kind <= LINE_H6) return a->fg_heading;
    if (kind == LINE_CODE)                   return a->fg_muted;
    if (style & STYLE_LINK)                  return a->fg_link;
    if (style & STYLE_CODE)                  return a->fg_muted;
    if (kind == LINE_QUOTE)                  return a->fg_quote;
    return a->fg;
}

static int styled_run(App* a, LineKind kind,
                      const char* p, const unsigned char* st, size_t n,
                      int x, int y_baseline, bool draw)
{
    int x_start = x;
    size_t i = 0;
    while (i < n) {
        unsigned char s = st[i];
        size_t j = i + 1;
        while (j < n && st[j] == s) j++;
        Font* f = pick_font(a, kind, s);
        int run_w = font_measure(f, p + i, j - i);
        if (draw) {
            SDL_Color c = pick_color(a, kind, s);
            font_draw_line(f, p + i, j - i, x, y_baseline, c);

            /* Track wiki/link click hit for preview-mode navigation. */
            if ((s & STYLE_LINK) && !a->edit_mode) {
                if (a->hit_count >= a->hit_cap) {
                    a->hit_cap = a->hit_cap ? a->hit_cap * 2 : 16;
                    a->hits = realloc(a->hits,
                                      a->hit_cap * sizeof(*a->hits));
                }
                a->hits[a->hit_count++] = (struct ClickHit){
                    .rect = { x, y_baseline - font_ascent(f),
                              run_w, font_line_height(f) },
                    .byte_start = (size_t)((p + i) - a->doc.data),
                    .kind = HIT_WIKI,
                };
            }
        }
        x += run_w;
        i = j;
    }
    return x - x_start;
}

static void render_line(App* a, const MdLine* line, int* y_inout, bool draw)
{
    Font* base       = pick_font(a, line->kind, 0);
    const int lh     = line_step(a, base);
    const int xL     = doc_x_left(a);
    const int wrap_r = doc_x_right(a) - MARGIN_X;
    int x_start      = xL + MARGIN_X + line->indent * INDENT_PX;
    int y            = *y_inout;

    if (line->kind == LINE_BLANK) { *y_inout = y + lh / 2; return; }

    if (line->kind == LINE_IMAGE) {
        char src[512];
        size_t n = line->len < sizeof(src) - 1 ? line->len : sizeof(src) - 1;
        memcpy(src, a->doc.data + line->start, n);
        src[n] = 0;

        char path[1024];
        int is_abs = (src[0] == '/' || src[0] == '\\' ||
                      (n >= 2 && src[1] == ':'));
        if (is_abs) {
            snprintf(path, sizeof path, "%s", src);
        } else {
            char dir[1024];
            snprintf(dir, sizeof dir, "%s", a->note_path ? a->note_path : "");
            char* slash = strrchr(dir, '/');
            char* bs    = strrchr(dir, '\\');
            char* last  = slash > bs ? slash : bs;
            if (last) *last = 0; else dir[0] = 0;
            snprintf(path, sizeof path, "%s/%s", dir, src);
        }

        int img_w = 0, img_h = 0;
        SDL_Texture* tex = image_cache_get(a->imgcache, path, &img_w, &img_h);
        if (tex) {
            int max_w  = doc_x_right(a) - xL - 2 * MARGIN_X;
            int draw_w = img_w;
            int draw_h = img_h;
            if (draw_w > max_w) {
                draw_h = (int)((double)draw_h * max_w / draw_w);
                draw_w = max_w;
            }
            if (draw) {
                SDL_Rect dst = { x_start, y, draw_w, draw_h };
                SDL_RenderCopy(a->renderer, tex, NULL, &dst);
            }
            *y_inout = y + draw_h + lh / 4;
        } else {
            char placeholder[600];
            snprintf(placeholder, sizeof placeholder, "[image not found: %s]", src);
            if (draw) font_draw_line(a->font_code, placeholder, strlen(placeholder),
                                     x_start, y + font_ascent(a->font_code),
                                     a->fg_muted);
            *y_inout = y + lh;
        }
        return;
    }

    /* The code-row chrome bg uses the doc right edge so it pulls in when the
     * outline panel is pinned. */
    #define ROW_CHROME(y_row)                                                  \
        do {                                                                   \
            if (draw && line->kind == LINE_CODE) {                             \
                SDL_Rect bg = { xL + MARGIN_X / 2, (y_row),                    \
                                doc_x_right(a) - xL - MARGIN_X, lh };          \
                SDL_SetRenderDrawColor(a->renderer,                            \
                    a->bg_code.r, a->bg_code.g, a->bg_code.b, a->bg_code.a);   \
                SDL_RenderFillRect(a->renderer, &bg);                          \
            }                                                                  \
            if (draw && line->kind == LINE_QUOTE) {                            \
                SDL_Rect bar = { xL + MARGIN_X - 8, (y_row), 3, lh };          \
                SDL_SetRenderDrawColor(a->renderer,                            \
                    a->fg_quote.r, a->fg_quote.g, a->fg_quote.b, 180);         \
                SDL_RenderFillRect(a->renderer, &bar);                         \
            }                                                                  \
        } while (0)

    ROW_CHROME(y);

    static const char BULLET[]  = "\xe2\x80\xa2  ";
    static const char CHK_OPEN[] = "\xe2\x98\x90  ";    /* ☐ */
    static const char CHK_DONE[] = "\xe2\x98\x91  ";    /* ☑ */
    if (line->kind == LINE_LIST) {
        if (draw) font_draw_line(a->font_body, BULLET, sizeof BULLET - 1,
                                 x_start - INDENT_PX, y + font_ascent(base),
                                 a->fg_muted);
    } else if (line->kind == LINE_LIST_TASK_OPEN ||
               line->kind == LINE_LIST_TASK_DONE) {
        const char* gl = (line->kind == LINE_LIST_TASK_DONE)
                         ? CHK_DONE : CHK_OPEN;
        size_t      gn = sizeof CHK_OPEN - 1;
        SDL_Color   gc = (line->kind == LINE_LIST_TASK_DONE)
                         ? a->fg_link : a->fg_muted;
        int gx = x_start - INDENT_PX;
        if (draw) font_draw_line(a->font_body, gl, gn,
                                 gx, y + font_ascent(base), gc);
        /* Click target: a tight box around the glyph. */
        if (draw && line->task_mark_off > 0) {
            int gw = font_measure(a->font_body, gl, 3);    /* just the box */
            if (a->hit_count >= a->hit_cap) {
                a->hit_cap = a->hit_cap ? a->hit_cap * 2 : 16;
                a->hits = realloc(a->hits, a->hit_cap * sizeof(*a->hits));
            }
            a->hits[a->hit_count++] = (struct ClickHit){
                .rect = { gx, y, gw, lh },
                .byte_start = line->task_mark_off,
                .kind = HIT_TASK,
            };
        }
    }

    if (line->len == 0) { *y_inout = y + lh; return; }

    const char*          data  = a->doc.data  + line->start;
    const unsigned char* style = a->doc.style + line->start;
    size_t               len   = line->len;

    int x = x_start;
    int row_y = y;
    size_t row_start = 0;            /* first byte of the visual row */
    size_t i = 0;
    while (i < len) {
        if (x == x_start) {
            while (i < len && data[i] == ' ') i++;
            if (i >= len) break;
            row_start = i;
        }
        size_t w_start = i;
        while (i < len && data[i] != ' ') i++;
        size_t w_len = i - w_start;

        int w = styled_run(a, line->kind, data + w_start, style + w_start,
                           w_len, 0, 0, false);

        if (x != x_start && x + w > wrap_r) {
            /* Close the wrapped-off row before starting the next. */
            if (draw) {
                if (a->preview_row_count >= a->preview_row_cap) {
                    a->preview_row_cap = a->preview_row_cap
                        ? a->preview_row_cap * 2 : 64;
                    a->preview_rows = realloc(a->preview_rows,
                        a->preview_row_cap * sizeof(*a->preview_rows));
                }
                a->preview_rows[a->preview_row_count++] = (struct PreviewRow){
                    .y = row_y, .lh = lh, .x_start = x_start,
                    .font = base,
                    .byte_start = line->start + row_start,
                    .byte_end   = line->start + w_start,
                };
            }
            y += lh;
            row_y = y;
            ROW_CHROME(y);
            x = x_start;
            i = w_start;
            continue;
        }
        /* Find-match highlight in preview. For each match overlapping
         * this word, we size the rect to just the matched bytes (not the
         * whole word) by measuring the prefix and the match itself with
         * the same styled-run logic the text uses. */
        if (draw && a->search_mode != 0 && a->search_count > 0) {
            size_t doc_w_start = line->start + w_start;
            size_t doc_w_end   = doc_w_start + w_len;
            for (size_t mi = 0; mi < a->search_count; ++mi) {
                size_t ms = a->search_matches[mi];
                size_t mlen = a->search_match_lens
                              ? a->search_match_lens[mi]
                              : a->search_qlen;
                size_t me = ms + mlen;
                if (me <= doc_w_start || ms >= doc_w_end) continue;

                /* Clip the match to this word run. */
                size_t hs = ms > doc_w_start ? ms : doc_w_start;
                size_t he = me < doc_w_end   ? me : doc_w_end;
                size_t off_s = hs - doc_w_start;
                size_t off_e = he - doc_w_start;

                int px_to_s = styled_run(a, line->kind,
                                         data + w_start, style + w_start,
                                         off_s, 0, 0, false);
                int px_to_e = styled_run(a, line->kind,
                                         data + w_start, style + w_start,
                                         off_e, 0, 0, false);
                if (px_to_e <= px_to_s) continue;

                bool current = ((int)mi == a->search_current);
                SDL_Rect r = { x + px_to_s, y,
                               px_to_e - px_to_s, lh };
                if (current) SDL_SetRenderDrawColor(a->renderer, 220,160, 60,140);
                else         SDL_SetRenderDrawColor(a->renderer, 180,130, 40, 90);
                SDL_RenderFillRect(a->renderer, &r);
            }
        }
        styled_run(a, line->kind, data + w_start, style + w_start,
                   w_len, x, y + font_ascent(base), draw);
        x += w;

        while (i < len && data[i] == ' ') {
            int sp = font_measure(base, " ", 1);
            if (x + sp > wrap_r) { i++; continue; }
            x += sp;
            i++;
        }
    }
    /* Close the final visual row of this logical line. */
    if (draw && line->len > 0) {
        if (a->preview_row_count >= a->preview_row_cap) {
            a->preview_row_cap = a->preview_row_cap
                ? a->preview_row_cap * 2 : 64;
            a->preview_rows = realloc(a->preview_rows,
                a->preview_row_cap * sizeof(*a->preview_rows));
        }
        a->preview_rows[a->preview_row_count++] = (struct PreviewRow){
            .y = row_y, .lh = lh, .x_start = x_start,
            .font = base,
            .byte_start = line->start + row_start,
            .byte_end   = line->start + len,
        };
    }
    *y_inout = y + lh;
    #undef ROW_CHROME
}

/* Map mouse (mx, my) to a byte offset in doc.data using the per-row map
 * built during the last preview render. Returns 0 if no row matches. */
static size_t preview_position_at(App* a, int mx, int my)
{
    for (size_t i = 0; i < a->preview_row_count; ++i) {
        struct PreviewRow* r = &a->preview_rows[i];
        if (my < r->y || my >= r->y + r->lh) continue;
        int x = r->x_start;
        size_t b = r->byte_start;
        while (b < r->byte_end) {
            size_t nxt = b + 1;
            while (nxt < r->byte_end &&
                   ((unsigned char)a->doc.data[nxt] & 0xC0) == 0x80) nxt++;
            int cw = font_measure(r->font, a->doc.data + b, nxt - b);
            if (x + cw / 2 >= mx) return b;
            x += cw;
            b = nxt;
        }
        return r->byte_end;
    }
    return 0;
}

/* Draw the translucent selection rectangle over preview text. Walks the
 * row map, intersecting with [sel_lo, sel_hi). */
static void render_preview_selection(App* a)
{
    if (a->preview_sel_start < 0) return;
    size_t lo = (size_t)a->preview_sel_start, hi = a->preview_sel_end;
    if (lo > hi) { size_t t = lo; lo = hi; hi = t; }
    if (lo == hi) return;

    SDL_SetRenderDrawColor(a->renderer,
        a->bg_selection.r, a->bg_selection.g,
        a->bg_selection.b, a->bg_selection.a);
    for (size_t i = 0; i < a->preview_row_count; ++i) {
        struct PreviewRow* r = &a->preview_rows[i];
        if (hi <= r->byte_start || lo >= r->byte_end) continue;
        size_t s = lo > r->byte_start ? lo : r->byte_start;
        size_t e = hi < r->byte_end   ? hi : r->byte_end;
        int sx = r->x_start +
            font_measure(r->font, a->doc.data + r->byte_start,
                         s - r->byte_start);
        int ex = r->x_start +
            font_measure(r->font, a->doc.data + r->byte_start,
                         e - r->byte_start);
        SDL_Rect rect = { sx, r->y, ex - sx, r->lh };
        SDL_RenderFillRect(a->renderer, &rect);
    }
}

/* Draw the frontmatter properties pill at the top of preview. Returns the
 * total y-advance (0 if no pill was drawn). When `draw` is false, only the
 * height is computed — used by the scroll math. */
static int render_frontmatter_pill(App* a, int x_left, int y_top, bool draw)
{
    if (!a->fm_present) return 0;
    if (!a->fm_title[0] && !a->fm_tags_csv[0]) return 0;

    int xL = x_left + MARGIN_X;
    int xR = doc_x_right(a) - MARGIN_X;
    int y  = y_top;
    int rh = font_line_height(a->font_body);

    /* Top divider. */
    if (draw) {
        SDL_SetRenderDrawColor(a->renderer,
            a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 60);
        SDL_Rect d = { xL, y, xR - xL, 1 };
        SDL_RenderFillRect(a->renderer, &d);
    }
    y += 8;

    /* Title (h2 size). */
    if (a->fm_title[0]) {
        if (draw) {
            font_draw_line(a->font_h2, a->fm_title, strlen(a->fm_title),
                           xL, y + font_ascent(a->font_h2),
                           a->fg_heading);
        }
        y += font_line_height(a->font_h2) + 4;
    }

    /* Tags as colored pills. Records rects on the App during the draw pass
     * so the click handler can pivot a chip click to vault search. */
    if (draw) a->fm_chip_count = 0;
    if (a->fm_tags_csv[0]) {
        int cx = xL;
        const char* p = a->fm_tags_csv;
        while (*p) {
            const char* s = p;
            while (*p && *p != ' ') p++;
            size_t n = (size_t)(p - s);
            char chip[80];
            int chiplen = snprintf(chip, sizeof chip, "#%.*s", (int)n, s);
            int cw = font_measure(a->font_body, chip, chiplen);
            int chip_w = cw + 12;
            int chip_h = rh + 2;
            if (cx + chip_w > xR) {
                cx = xL;
                y += chip_h + 4;
            }
            if (draw) {
                SDL_Rect r = { cx, y, chip_w, chip_h };
                SDL_SetRenderDrawColor(a->renderer,
                    a->bg_sidebar_active.r, a->bg_sidebar_active.g,
                    a->bg_sidebar_active.b, 200);
                SDL_RenderFillRect(a->renderer, &r);
                font_draw_line(a->font_body, chip, chiplen,
                               cx + 6,
                               y + font_ascent(a->font_body),
                               a->fg_link);
                if (a->fm_chip_count <
                    (int)(sizeof a->fm_chip_hits / sizeof a->fm_chip_hits[0]))
                {
                    struct FmChip* h = &a->fm_chip_hits[a->fm_chip_count++];
                    h->rect = r;
                    size_t tn = n;
                    if (tn >= sizeof h->tag) tn = sizeof h->tag - 1;
                    memcpy(h->tag, s, tn);
                    h->tag[tn] = 0;
                }
            }
            cx += chip_w + 6;
            while (*p == ' ') p++;
        }
        y += rh + 6;
    }

    /* Bottom divider. */
    if (draw) {
        SDL_SetRenderDrawColor(a->renderer,
            a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 60);
        SDL_Rect d = { xL, y, xR - xL, 1 };
        SDL_RenderFillRect(a->renderer, &d);
    }
    y += 12;

    return y - y_top;
}

/* Render a contiguous run of LINE_TABLE_HEAD/ROW lines as a grid. Two
 * passes: first collect column count + max pixel-width per column, then
 * lay out cells with padding + a header divider underline. */
#define MD_PREV_TABLE_COLS 12

static void render_table_run(App* a, size_t i0, size_t i1,
                             int* y_inout, bool draw)
{
    Font* f       = a->font_body;
    Font* fb      = a->font_body_bold;
    int   lh      = line_step(a, f);
    int   xL      = doc_x_left(a) + MARGIN_X;
    int   pad     = 14;     /* per-cell horizontal padding */
    int   row_pad = 4;      /* extra vertical breathing room per row */
    int   row_h   = lh + 2 * row_pad;

    int col_w[MD_PREV_TABLE_COLS] = {0};
    int col_count = 0;

    for (size_t i = i0; i < i1; ++i) {
        const MdLine* l = &a->doc.lines[i];
        Font* cf = (l->kind == LINE_TABLE_HEAD) ? fb : f;
        const char* data = a->doc.data + l->start;
        size_t len = l->len;
        size_t cs = 0;
        int col = 0;
        for (size_t j = 0; j <= len && col < MD_PREV_TABLE_COLS; ++j) {
            if (j == len || data[j] == '\t') {
                int w = font_measure(cf, data + cs, j - cs);
                if (w > col_w[col]) col_w[col] = w;
                col++;
                cs = j + 1;
            }
        }
        if (col > col_count) col_count = col;
    }

    int total_w = 0;
    for (int c = 0; c < col_count; ++c) total_w += col_w[c] + 2 * pad;

    int y = *y_inout;
    for (size_t i = i0; i < i1; ++i) {
        const MdLine* l = &a->doc.lines[i];
        bool is_head = (l->kind == LINE_TABLE_HEAD);
        Font* cf = is_head ? fb : f;
        const char* data = a->doc.data + l->start;
        size_t len = l->len;

        if (draw) {
            /* Subtle alt-row tint for body rows after the header. */
            if (!is_head && ((i - i0) % 2 == 1)) {
                SDL_SetRenderDrawColor(a->renderer,
                    a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 12);
                SDL_Rect bg = { xL, y, total_w, row_h };
                SDL_RenderFillRect(a->renderer, &bg);
            }
        }

        int cx = xL;
        size_t cs = 0;
        int col = 0;
        for (size_t j = 0; j <= len && col < col_count; ++j) {
            if (j == len || data[j] == '\t') {
                if (draw) {
                    font_draw_line(cf, data + cs, j - cs,
                                   cx + pad,
                                   y + row_pad + font_ascent(cf),
                                   is_head ? a->fg_heading : a->fg);
                    /* Vertical separator on the right edge of every cell
                     * except the last. */
                    if (col + 1 < col_count) {
                        SDL_SetRenderDrawColor(a->renderer,
                            a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 40);
                        SDL_RenderDrawLine(a->renderer,
                            cx + col_w[col] + 2 * pad - 1, y,
                            cx + col_w[col] + 2 * pad - 1, y + row_h);
                    }
                }
                cx += col_w[col] + 2 * pad;
                col++;
                cs = j + 1;
            }
        }

        if (draw) {
            /* Underline header heavier; thin separator between body rows. */
            SDL_SetRenderDrawColor(a->renderer,
                a->fg_muted.r, a->fg_muted.g, a->fg_muted.b,
                is_head ? 140 : 30);
            SDL_RenderDrawLine(a->renderer,
                xL, y + row_h - 1, xL + total_w, y + row_h - 1);
            /* Outer top border on the very first row. */
            if (i == i0) {
                SDL_RenderDrawLine(a->renderer,
                    xL, y, xL + total_w, y);
            }
        }
        y += row_h;
    }
    *y_inout = y + lh / 3;     /* a bit of breathing space after the table */
}

static int render_preview(App* a, bool draw)
{
    int y = doc_y_top(a) - a->scroll_y;
    y += render_frontmatter_pill(a, doc_x_left(a), y, draw);
    for (size_t i = 0; i < a->doc.line_count; ++i) {
        const MdLine* l = &a->doc.lines[i];
        if (l->kind == LINE_TABLE_HEAD || l->kind == LINE_TABLE_ROW) {
            size_t end = i + 1;
            while (end < a->doc.line_count) {
                LineKind k = a->doc.lines[end].kind;
                if (k != LINE_TABLE_HEAD && k != LINE_TABLE_ROW) break;
                end++;
            }
            render_table_run(a, i, end, &y, draw);
            i = end - 1;
            continue;
        }
        render_line(a, l, &y, draw);
    }
    return y + a->scroll_y - doc_y_top(a);
}

/* ----------------------------- editor render ---------------------------- */

/* "Live preview" edit mode: heading lines get the heading fonts, body lines
 * use the proportional body font (so prose reads like the preview pane),
 * fenced code blocks fall back to monospace. Inline `**bold**`, `*italic*`,
 * `` `code` `` are painted with the matching style variant by
 * pick_edit_inline_font_for. Per-line variable line-height; the cursor
 * position math walks lines summing heights. */
static Font* edit_line_font(const App* a, const char* s, size_t len)
{
    int n = 0;
    while (n < 6 && n < (int)len && s[n] == '#') n++;
    if (n > 0 && n < (int)len && s[n] == ' ') {
        if (n == 1) return a->font_h1;
        if (n == 2) return a->font_h2;
        return a->font_h3;
    }
    return a->font_body;
}

/* Forward decl — definition lives further down with the auto-pair helpers. */
static int is_word_char(unsigned char c);

/* Per-byte inline style flags for edit-mode body lines. */
#define ES_BOLD    (1u << 0)
#define ES_ITALIC  (1u << 1)
#define ES_CODE    (1u << 2)
#define ES_TAG     (1u << 3)     /* #tag — colored like a link */

/* Is this line a ``` fence marker? Optional leading whitespace, then 3+
 * backticks. Anything after the backticks (e.g. ```c) is still a fence. */
static int is_fence_line(const char* s, size_t n)
{
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    int count = 0;
    while (i < n && s[i] == '`') { count++; i++; }
    return count >= 3;
}

/* Walk a line and assign per-byte style flags based on Markdown markers
 * (`**`, `*`, `` ` ``, `#tag`). The markers themselves get the style of
 * the span they bound, so the whole `**word**` reads as a continuous bold
 * run. Tags consume their `#` and trailing word chars (incl. `/-_`). */
static void compute_edit_styles(const char* s, size_t n, unsigned char* st)
{
    int bold = 0, italic = 0, code = 0;
    size_t i = 0;
    while (i < n) {
        if (code) {
            st[i] = ES_CODE;
            if (s[i] == '`') code = 0;
            i++; continue;
        }
        if (s[i] == '`') {
            code = 1;
            st[i] = ES_CODE;
            i++; continue;
        }
        if (i + 1 < n && s[i] == '*' && s[i+1] == '*') {
            unsigned char c = ES_BOLD | (italic ? ES_ITALIC : 0);
            st[i] = c; st[i+1] = c;
            bold = !bold;
            i += 2; continue;
        }
        if (s[i] == '*') {
            unsigned char c = ES_ITALIC | (bold ? ES_BOLD : 0);
            st[i] = c;
            italic = !italic;
            i++; continue;
        }
        /* #tag: `#` at start-of-line or after whitespace/`(`/`[`, followed
         * by a word char. Consumes the run of word chars (plus `-`, `/`).
         * Skipped on heading lines because edit_line_font sends those down
         * a different path that never calls compute_edit_styles. */
        if (s[i] == '#') {
            bool boundary = (i == 0) ||
                s[i-1] == ' ' || s[i-1] == '\t' ||
                s[i-1] == '(' || s[i-1] == '[';
            if (boundary && i + 1 < n &&
                (is_word_char((unsigned char)s[i+1]) || s[i+1] == '/'))
            {
                st[i] = ES_TAG;
                size_t k = i + 1;
                while (k < n &&
                       (is_word_char((unsigned char)s[k]) ||
                        s[k] == '-' || s[k] == '/'))
                {
                    st[k] = ES_TAG;
                    k++;
                }
                i = k; continue;
            }
        }
        st[i] = (bold ? ES_BOLD : 0) | (italic ? ES_ITALIC : 0);
        i++;
    }

    /* Overlay: task-list checkbox markers. `^[ws]*[-*+] \[([ xX])\]` →
     * style the three bracketed chars as ES_TAG so they pop. */
    size_t p = 0;
    while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
    if (p + 4 < n &&
        (s[p] == '-' || s[p] == '*' || s[p] == '+') &&
        s[p + 1] == ' ' &&
        s[p + 2] == '[' &&
        (s[p + 3] == ' ' || s[p + 3] == 'x' || s[p + 3] == 'X') &&
        s[p + 4] == ']')
    {
        st[p + 2] = ES_TAG;
        st[p + 3] = ES_TAG;
        st[p + 4] = ES_TAG;
    }
}

/* Pick the inline font given a line's base font: body lines use the
 * proportional body family; fenced-code lines use the monospace code family;
 * inline `code` is always monospace. Headings have no bold/italic variants
 * at the heading size, so they reuse `base` for non-code styles. */
static Font* pick_edit_inline_font_for(const App* a, Font* base, unsigned char s)
{
    if (s & ES_CODE) return a->font_code;
    int b = (s & ES_BOLD) != 0, i = (s & ES_ITALIC) != 0;
    if (base == a->font_body || base == a->font_body_bold ||
        base == a->font_body_italic || base == a->font_body_bold_italic)
    {
        if (b && i) return a->font_body_bold_italic;
        if (b)      return a->font_body_bold;
        if (i)      return a->font_body_italic;
        return a->font_body;
    }
    if (base == a->font_code) {
        if (b && i) return a->font_code_bold_italic;
        if (b)      return a->font_code_bold;
        if (i)      return a->font_code_italic;
        return a->font_code;
    }
    /* Headings: keep the heading face for everything except inline code
     * (already returned above). */
    return base;
}

/* Auto-pair table for typed openers and the corresponding closer.
 * Returns 0 if `c` is not an opener we want to pair. */
static char pair_closer_for(char c)
{
    switch (c) {
        case '(':  return ')';
        case '[':  return ']';
        case '{':  return '}';
        case '"':  return '"';
        case '\'': return '\'';
        case '`':  return '`';
        default:   return 0;
    }
}

/* True if `c` is a typed character that should "skip over" an existing
 * matching char immediately to the right of the cursor (so typing `)` when
 * sitting at `|)` just moves the cursor right instead of inserting). */
static int is_pair_closer_char(char c)
{
    return c == ')' || c == ']' || c == '}'
        || c == '"' || c == '\'' || c == '`';
}

static int is_word_char(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z') || c == '_'
        || c >= 0x80;     /* treat any UTF-8 continuation/lead as word */
}

/* If `pos` lies inside a `[[name]]` wiki-link in the buffer, copy the inner
 * name into `out` (NUL-terminated, capped at `cap`) and return its length.
 * Returns 0 if `pos` is not inside any link. The link must be on a single
 * line (no newline between the brackets). */
static size_t edit_wiki_link_at(const Buffer* b, size_t pos,
                                char* out, size_t cap)
{
    if (pos > b->len || cap == 0) return 0;
    /* Walk backward to find `[[`. Stop on newline, `]]`, or buffer start. */
    size_t start = pos;
    while (start > 0) {
        char c = b->data[start - 1];
        if (c == '\n') return 0;
        if (start >= 2 && b->data[start - 2] == '[' && b->data[start - 1] == '[')
            break;
        if (start >= 2 && b->data[start - 2] == ']' && b->data[start - 1] == ']')
            return 0;
        start--;
    }
    if (start < 2 ||
        b->data[start - 2] != '[' || b->data[start - 1] != '[')
        return 0;
    /* Walk forward to find `]]`. Stop on newline or `[[`. */
    size_t end = pos;
    while (end + 1 < b->len) {
        if (b->data[end] == '\n') return 0;
        if (b->data[end] == ']' && b->data[end + 1] == ']') break;
        if (b->data[end] == '[' && b->data[end + 1] == '[') return 0;
        end++;
    }
    if (end + 2 > b->len ||
        b->data[end] != ']' || b->data[end + 1] != ']')
        return 0;
    if (end <= start) return 0;
    size_t n = end - start;
    if (n >= cap) n = cap - 1;
    memcpy(out, b->data + start, n);
    out[n] = 0;
    return n;
}

/* Forward decl — defined later. */
static int strieq(const char* x, const char* y);

/* Resolve a wiki-link target to a vault item path (case-insensitive basename
 * match against vault items, sans `.md`) and open it. Returns true if a
 * matching item was found and load_note was called. */
static bool follow_wiki_target(App* a, const char* name)
{
    if (!name || !*name) return false;
    for (size_t v = 0; v < a->vault.count; ++v) {
        if (a->vault.items[v].is_dir) continue;
        char base[256];
        snprintf(base, sizeof base, "%s", a->vault.items[v].name);
        size_t bl = strlen(base);
        if (bl > 3 && (base[bl-3] == '.') &&
            (base[bl-2] == 'm' || base[bl-2] == 'M') &&
            (base[bl-1] == 'd' || base[bl-1] == 'D'))
            base[bl - 3] = 0;
        if (strieq(base, name)) {
            if (!confirm_discard(a)) return false;
            load_note(a, a->vault.items[v].path);
            return true;
        }
    }
    fprintf(stderr, "wiki: no vault item matches [[%s]]\n", name);
    return false;
}

/* Smart-Enter: if the cursor is on a list/quote line, continue the prefix
 * on a new line. If the line is JUST the prefix (empty bullet), delete the
 * prefix and insert a plain newline (outdent / break list). Returns true
 * if it handled the keypress; the caller falls back to a plain newline. */
static bool smart_enter(App* a)
{
    Buffer* b = &a->buf;
    if (buffer_has_selection(b)) return false;

    /* Find current line bounds. */
    size_t ls = b->cursor;
    while (ls > 0 && b->data[ls - 1] != '\n') ls--;
    size_t le = b->cursor;
    while (le < b->len && b->data[le] != '\n') le++;

    /* Walk leading whitespace. */
    size_t ws_end = ls;
    while (ws_end < le && (b->data[ws_end] == ' ' || b->data[ws_end] == '\t'))
        ws_end++;
    if (ws_end >= le) return false;

    /* Detect marker: `-`, `*`, `+`, `>` followed by space, or `\d+. `. */
    size_t marker_end = ws_end;
    bool   is_ordered = false;
    long   ordered_num = 0;
    char   c = b->data[marker_end];
    if ((c == '-' || c == '*' || c == '+' || c == '>') &&
        marker_end + 1 < le && b->data[marker_end + 1] == ' ')
    {
        marker_end += 2;
    } else if (c >= '0' && c <= '9') {
        size_t p = marker_end;
        while (p < le && b->data[p] >= '0' && b->data[p] <= '9') {
            ordered_num = ordered_num * 10 + (b->data[p] - '0');
            p++;
        }
        if (p + 1 < le && b->data[p] == '.' && b->data[p + 1] == ' ') {
            marker_end = p + 2;
            is_ordered = true;
        } else {
            return false;
        }
    } else {
        return false;
    }

    /* Empty bullet (cursor at end-of-marker, no body): outdent. Cursor must
     * be AT or AFTER marker_end so we don't outdent when typing in the
     * middle of "    1. |"; otherwise normal split. */
    if (b->cursor >= marker_end && le == marker_end) {
        size_t to_del = marker_end - ls;
        for (size_t i = 0; i < to_del; ++i) buffer_delete_back(b);
        buffer_insert(b, "\n", 1);
        return true;
    }

    /* Build the prefix to repeat: "\n" + leading-whitespace + marker. */
    char prefix[256];
    size_t pl = 0;
    prefix[pl++] = '\n';
    size_t ws_n = ws_end - ls;
    if (ws_n > sizeof prefix - 32) ws_n = sizeof prefix - 32;
    memcpy(prefix + pl, b->data + ls, ws_n);
    pl += ws_n;
    if (is_ordered) {
        int wrote = snprintf(prefix + pl, sizeof prefix - pl,
                             "%ld. ", ordered_num + 1);
        if (wrote > 0 && (size_t)wrote < sizeof prefix - pl)
            pl += (size_t)wrote;
    } else {
        prefix[pl++] = b->data[ws_end];     /* marker char */
        prefix[pl++] = ' ';
    }
    buffer_insert(b, prefix, pl);
    return true;
}

/* Reusable scratch buffer for per-line style flags. */
static unsigned char* g_styles_buf = NULL;
static size_t         g_styles_cap = 0;
static unsigned char* styles_for(size_t n)
{
    if (n > g_styles_cap) {
        g_styles_cap = n + 64;
        g_styles_buf = realloc(g_styles_buf, g_styles_cap);
    }
    return g_styles_buf;
}

/* True if `f` is one of the heading fonts (no per-style variants exist for
 * those, so the per-byte walk degenerates to a single measure). */
static bool is_heading_font(const App* a, Font* f)
{
    return f == a->font_h1 || f == a->font_h2 || f == a->font_h3;
}

/* x-advance from byte 0 to byte `to_byte` of an edit-mode line, walking
 * style runs with per-style fonts. Headings short-circuit to a single
 * font_measure since their family has no bold/italic variants. When
 * `in_fence`, the entire line is treated as code (no inline marker scan). */
static int edit_line_x_at(const App* a, const char* data, size_t len,
                          size_t to_byte, Font* base, bool in_fence)
{
    if (to_byte > len) to_byte = len;
    if (is_heading_font(a, base))
        return font_measure(base, data, to_byte);

    unsigned char* st = styles_for(len);
    if (in_fence) memset(st, ES_CODE, len);
    else          compute_edit_styles(data, len, st);
    int x = 0;
    size_t i = 0;
    while (i < to_byte) {
        unsigned char s = st[i];
        size_t j = i + 1;
        while (j < to_byte && st[j] == s) j++;
        Font* f = pick_edit_inline_font_for(a, base, s);
        x += font_measure(f, data + i, j - i);
        i = j;
    }
    return x;
}

/* Draw an edit-mode line as styled runs. Returns total advance. */
static int edit_line_draw(const App* a, const char* data, size_t len,
                          int x, int y_baseline, SDL_Color color, Font* base,
                          bool in_fence)
{
    if (is_heading_font(a, base)) {
        font_draw_line(base, data, len, x, y_baseline, color);
        return font_measure(base, data, len);
    }
    unsigned char* st = styles_for(len);
    if (in_fence) memset(st, ES_CODE, len);
    else          compute_edit_styles(data, len, st);
    int x_start = x;
    size_t i = 0;
    while (i < len) {
        unsigned char s = st[i];
        size_t j = i + 1;
        while (j < len && st[j] == s) j++;
        Font* f = pick_edit_inline_font_for(a, base, s);
        SDL_Color c = color;
        if      (s & ES_CODE) c = a->fg_muted;
        else if (s & ES_TAG)  c = a->fg_link;
        font_draw_line(f, data + i, j - i, x, y_baseline, c);
        x += font_measure(f, data + i, j - i);
        i = j;
    }
    return x - x_start;
}

static int render_editor(App* a)
{
    int xL = doc_x_left(a);
    int y  = doc_y_top(a) - a->scroll_y;
    Buffer* b = &a->buf;

    SDL_Rect clip = { xL, chrome_bar_h(a), doc_x_right(a) - xL,
                      a->win_h - status_bar_h(a) - chrome_bar_h(a) };
    SDL_RenderSetClipRect(a->renderer, &clip);

    bool   has_sel = buffer_has_selection(b);
    size_t sl = 0, sh = 0;
    if (has_sel) buffer_get_selection(b, &sl, &sh);

    int    total_h = 0;
    size_t n_lines = buffer_line_count(b);
    bool   in_fence = false;     /* state machine carried across lines */

    for (size_t li = 0; li < n_lines; ++li) {
        size_t ls   = buffer_line_start(b, li);
        size_t le   = buffer_line_end(b, li);
        size_t llen = le - ls;

        Font* lf = edit_line_font(a, b->data + ls, llen);
        int   lh = line_step(a, lf);
        bool  visible = (y + lh > 0) && (y < a->win_h);

        /* Fence-line itself renders in code style; THEN flips state. */
        bool line_is_fence = is_fence_line(b->data + ls, llen);
        bool draw_in_fence = in_fence || line_is_fence;

        if (visible) {
            if (has_sel && sl < le && sh > ls) {
                size_t s = sl > ls ? sl : ls;
                size_t e = sh < le ? sh : le;
                int sx = xL + MARGIN_X +
                    edit_line_x_at(a, b->data + ls, llen,
                                   s > ls ? s - ls : 0, lf, draw_in_fence);
                int ex = xL + MARGIN_X +
                    edit_line_x_at(a, b->data + ls, llen,
                                   e > ls ? e - ls : 0, lf, draw_in_fence);
                if (sh > le) ex += font_measure(lf, " ", 1);
                SDL_Rect r = { sx, y, ex - sx, lh };
                SDL_SetRenderDrawColor(a->renderer,
                    a->bg_selection.r, a->bg_selection.g,
                    a->bg_selection.b, a->bg_selection.a);
                SDL_RenderFillRect(a->renderer, &r);
            }

            if (llen > 0) {
                bool is_h = (lf == a->font_h1 || lf == a->font_h2 || lf == a->font_h3);
                SDL_Color c = is_h ? a->fg_heading : a->fg;
                edit_line_draw(a, b->data + ls, llen,
                               xL + MARGIN_X, y + font_ascent(lf), c, lf,
                               draw_in_fence);
            }

            if (b->cursor >= ls && b->cursor <= le && cursor_visible_now(a)) {
                int cx = xL + MARGIN_X +
                    edit_line_x_at(a, b->data + ls, llen, b->cursor - ls,
                                   lf, draw_in_fence);
                SDL_Rect cr = { cx, y, 2, lh };
                SDL_SetRenderDrawColor(a->renderer,
                    a->fg_cursor.r, a->fg_cursor.g, a->fg_cursor.b, 255);
                SDL_RenderFillRect(a->renderer, &cr);
            }
        }

        if (line_is_fence) in_fence = !in_fence;

        y       += lh;
        total_h += lh;
    }

    SDL_RenderSetClipRect(a->renderer, NULL);
    return total_h;
}

/* ----------------------------- chrome ----------------------------------- */

/* Most icons render via SVG (icons.c — settings, find, sidebar, outline,
 * folder, file, caret). The two procedurally-drawn helpers below survive
 * because they're parameterized in ways the icon module doesn't yet
 * handle: chevron takes a direction (left/right) for settings adjusters,
 * fill_tri is reused by other primitives. */

#if 0   /* superseded by icon_draw(ICON_SETTINGS, ...) — kept for reference. */
static void draw_icon_settings(SDL_Renderer* r, int x, int y, int sz, SDL_Color c)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    int pad      = sz / 5;
    int rail_w   = sz - 2 * pad;
    int rail_h   = 2;
    int knob_d   = 6;
    int n        = 3;
    int slot     = rail_w / n + 2;          /* vertical spacing per slider */
    int total_h  = (n - 1) * slot + rail_h;
    int top_y    = y + (sz - total_h) / 2;
    int knob_x[3] = { 60, 30, 80 };          /* % positions */
    for (int i = 0; i < n; ++i) {
        int ly = top_y + i * slot;
        SDL_Rect rail = { x + pad, ly, rail_w, rail_h };
        SDL_RenderFillRect(r, &rail);
        int kx = x + pad + (rail_w - knob_d) * knob_x[i] / 100;
        SDL_Rect knob = { kx, ly + rail_h/2 - knob_d/2, knob_d, knob_d };
        /* Knob: filled with a 1px hole punched out for a "ring" look. */
        SDL_RenderFillRect(r, &knob);
        SDL_Rect hole = { kx + 2, ly + rail_h/2 - knob_d/2 + 2,
                          knob_d - 4, knob_d - 4 };
        if (hole.w > 0 && hole.h > 0) {
            /* Punch with the chrome bg so the knob reads as a ring. The bg
             * is approximated by alpha-blending the icon color to ~0;
             * cheap trick but with our flat backgrounds it works. */
            SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 0);
            SDL_RenderFillRect(r, &hole);
            SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
        }
    }
}

/* Outline / list icon — four evenly spaced rails, each indented at a
 * progressively deeper bullet column to suggest hierarchy. Bullet dots in
 * front of each rail anchor the eye and read as a tree. */
static void draw_icon_outline(SDL_Renderer* r, int x, int y, int sz, SDL_Color c)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    int pad    = sz / 5;
    int rail_h = 2;
    int rows   = 4;
    int max_w  = sz - 2 * pad;
    int gap    = (sz - 2 * pad - rows * rail_h) / (rows - 1);
    int dot_d  = 2;
    int dot_x  = x + pad;
    /* Per-row indents: 0, 1, 1, 0 — looks like a branch and a return. */
    int indents[4] = { 0, 4, 4, 0 };
    for (int i = 0; i < rows; ++i) {
        int ly = y + pad + i * (rail_h + gap);
        SDL_Rect dot = { dot_x + indents[i], ly, dot_d, rail_h };
        SDL_RenderFillRect(r, &dot);
        SDL_Rect rail = { dot_x + indents[i] + dot_d + 2, ly,
                          max_w - indents[i] - dot_d - 2, rail_h };
        SDL_RenderFillRect(r, &rail);
    }
}

/* Sidebar / panel icon — outlined rounded rect with a filled left or right
 * third indicating which side has the panel. The internal divider is a
 * 1px line so it reads cleanly even at small sizes. */
static void draw_icon_sidebar(SDL_Renderer* r, int x, int y, int sz,
                              SDL_Color c, int open)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    int pad    = sz / 6;
    int box_w  = sz - 2 * pad;
    int box_h  = sz - 2 * pad - 2;
    int box_x  = x + pad;
    int box_y  = y + pad + 1;
    SDL_Rect outer = { box_x, box_y, box_w, box_h };
    SDL_RenderDrawRect(r, &outer);
    int side_w = box_w * 2 / 5;
    if (open) {
        SDL_Rect side = { box_x, box_y, side_w, box_h };
        SDL_RenderFillRect(r, &side);
    } else {
        /* Closed: just the divider line — the document area stays empty so
         * the icon visually says "panel hidden". */
        SDL_RenderDrawLine(r, box_x + side_w, box_y,
                              box_x + side_w, box_y + box_h - 1);
    }
}

/* Magnifier — bigger circle, double-stroked, with a slightly wider handle
 * extending to the bottom-right corner. The lens reads as a real lens at
 * normal sizes, not just dots. */
static void draw_icon_find(SDL_Renderer* r, int x, int y, int sz, SDL_Color c)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    int pad    = sz / 6;
    int radius = (sz - 2 * pad) * 9 / 20;     /* bigger lens than before */
    int cx     = x + pad + radius + 1;
    int cy     = y + pad + radius + 1;
    for (int rad = radius; rad >= radius - 1; --rad) {
        int dx = rad, dy = 0, err = 0;
        while (dx >= dy) {
            SDL_RenderDrawPoint(r, cx + dx, cy + dy);
            SDL_RenderDrawPoint(r, cx + dy, cy + dx);
            SDL_RenderDrawPoint(r, cx - dy, cy + dx);
            SDL_RenderDrawPoint(r, cx - dx, cy + dy);
            SDL_RenderDrawPoint(r, cx - dx, cy - dy);
            SDL_RenderDrawPoint(r, cx - dy, cy - dx);
            SDL_RenderDrawPoint(r, cx + dy, cy - dx);
            SDL_RenderDrawPoint(r, cx + dx, cy - dy);
            if (err <= 0) { dy++; err += 2*dy + 1; }
            if (err > 0)  { dx--; err -= 2*dx + 1; }
        }
    }
    /* Handle: 3px-thick diagonal from the lens edge to the corner. */
    int hx0 = cx + (radius * 7) / 10;
    int hy0 = cy + (radius * 7) / 10;
    int hx1 = x + sz - pad - 1;
    int hy1 = y + sz - pad - 1;
    SDL_RenderDrawLine(r, hx0,     hy0,     hx1,     hy1);
    SDL_RenderDrawLine(r, hx0 + 1, hy0,     hx1 + 1, hy1);
    SDL_RenderDrawLine(r, hx0,     hy0 + 1, hx1,     hy1 + 1);
    SDL_RenderDrawLine(r, hx0 - 1, hy0,     hx1 - 1, hy1);
}
#endif   /* superseded icons */

/* Filled triangle by horizontal scanlines. p1/p2/p3 in any order. */
static void fill_tri(SDL_Renderer* r, int x1, int y1, int x2, int y2,
                     int x3, int y3)
{
    /* Sort by y so y1 <= y2 <= y3. */
    if (y2 < y1) { int t; t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    if (y3 < y1) { int t; t = y1; y1 = y3; y3 = t; t = x1; x1 = x3; x3 = t; }
    if (y3 < y2) { int t; t = y2; y2 = y3; y3 = t; t = x2; x2 = x3; x3 = t; }
    /* Long edge (y1 -> y3) and short edges (y1 -> y2, y2 -> y3). */
    for (int y = y1; y <= y3; ++y) {
        if (y3 == y1) break;
        int xa = x1 + (x3 - x1) * (y - y1) / (y3 - y1);
        int xb;
        if (y < y2) {
            if (y2 == y1) xb = x1;
            else          xb = x1 + (x2 - x1) * (y - y1) / (y2 - y1);
        } else {
            if (y3 == y2) xb = x2;
            else          xb = x2 + (x3 - x2) * (y - y2) / (y3 - y2);
        }
        if (xa > xb) { int t = xa; xa = xb; xb = t; }
        SDL_RenderDrawLine(r, xa, y, xb, y);
    }
}

/* draw_icon_chevron superseded by ICON_CHEVRON_LEFT / ICON_CHEVRON_RIGHT
 * in icons.c (SVG-rasterized, oversampled, AA). */

#if 0   /* superseded by ICON_CARET_*, ICON_FOLDER*, ICON_FILE in icons.c */
/* Tree caret: small filled triangle pointing right (collapsed) or down
 * (expanded). Used in the sidebar to mark folder collapse state. */
static void draw_icon_caret(SDL_Renderer* r, int x, int y, int sz,
                            int expanded, SDL_Color c)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    int pad = sz / 4;
    if (expanded) {
        /* ▼ */
        int top_y  = y + pad + 1;
        int bot_y  = y + sz - pad - 1;
        int left_x = x + pad;
        int right_x = x + sz - pad;
        int mid_x  = x + sz / 2;
        fill_tri(r, left_x, top_y, right_x, top_y, mid_x, bot_y);
    } else {
        /* ▶ */
        int left_x = x + pad + 1;
        int right_x = x + sz - pad - 1;
        int top_y  = y + pad;
        int bot_y  = y + sz - pad;
        int mid_y  = y + sz / 2;
        fill_tri(r, left_x, top_y, left_x, bot_y, right_x, mid_y);
    }
}

/* Folder icon. `expanded` shifts the front face down a touch + draws a
 * notch on the lid so the eye can tell open from closed without colour. */
static void draw_icon_folder(SDL_Renderer* r, int x, int y, int sz,
                             SDL_Color c, int expanded)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    int pad = sz / 5;
    int x0  = x + pad;
    int y0  = y + pad + 1;
    int w   = sz - 2 * pad;
    int h   = sz - 2 * pad - 2;
    int tab_w = w * 5 / 12;
    int tab_h = 2;

    /* Tab on top-left. */
    SDL_Rect tab = { x0, y0, tab_w, tab_h + 1 };
    SDL_RenderFillRect(r, &tab);

    /* Body outline (rounded-feeling rect). */
    SDL_Rect body = { x0, y0 + tab_h, w, h - tab_h };
    SDL_RenderDrawRect(r, &body);
    /* Inset a faint fill so closed folders feel solid. */
    if (!expanded) {
        SDL_Rect fill = { x0 + 1, y0 + tab_h + 1, w - 2, h - tab_h - 2 };
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, (Uint8)(c.a / 4));
        SDL_RenderFillRect(r, &fill);
    } else {
        /* Open: a light hairline on the lid suggesting an opened front. */
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
        SDL_RenderDrawLine(r, x0 + 2, y0 + tab_h + 2,
                              x0 + w - 3, y0 + tab_h + 2);
    }
}

/* File / page icon: rectangle with the upper-right corner folded over. */
static void draw_icon_file(SDL_Renderer* r, int x, int y, int sz, SDL_Color c)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    int pad = sz / 5;
    int x0  = x + pad + 1;
    int y0  = y + pad;
    int w   = sz - 2 * pad - 2;
    int h   = sz - 2 * pad;
    int fold = w / 3;

    /* Outline of the page. We draw the body as four lines so the corner
     * fold works correctly. */
    /* Top edge (stops short of the fold). */
    SDL_RenderDrawLine(r, x0,         y0, x0 + w - fold, y0);
    /* Diagonal fold. */
    SDL_RenderDrawLine(r, x0 + w - fold, y0, x0 + w,        y0 + fold);
    /* Right edge (starts after the fold). */
    SDL_RenderDrawLine(r, x0 + w, y0 + fold, x0 + w, y0 + h);
    /* Bottom edge. */
    SDL_RenderDrawLine(r, x0,     y0 + h,    x0 + w, y0 + h);
    /* Left edge. */
    SDL_RenderDrawLine(r, x0,     y0,        x0,     y0 + h);
    /* The fold itself (the folded-over flap shows as a small triangle). */
    SDL_RenderDrawLine(r, x0 + w - fold, y0,        x0 + w - fold, y0 + fold);
    SDL_RenderDrawLine(r, x0 + w - fold, y0 + fold, x0 + w,        y0 + fold);
}
#endif   /* superseded sidebar/file/folder/caret icons */

/* ---------- Title bar (custom decorations) ---------------------------- */

/* Window-control buttons in the title bar (right edge). */
enum TitleBarButton {
    TBB_NONE  = -1,
    TBB_MIN   = 0,
    TBB_MAX   = 1,
    TBB_CLOSE = 2,
};
#define TB_BTN_W 46

static const char* MENU_LABELS[4] = { "File", "Edit", "View", "Help" };

static int titlebar_button_at(const App* a, int mx, int my)
{
    int TBH = title_bar_h(a);
    if (my < 0 || my >= TBH) return TBB_NONE;
    int right = a->win_w;
    if (mx >= right - TB_BTN_W       && mx < right)            return TBB_CLOSE;
    if (mx >= right - 2 * TB_BTN_W   && mx < right - TB_BTN_W) return TBB_MAX;
    if (mx >= right - 3 * TB_BTN_W   && mx < right - 2*TB_BTN_W) return TBB_MIN;
    return TBB_NONE;
}

/* Hit-test against the menu items in the title bar. Returns 0..3 or -1.
 * Uses the rects stashed by render_titlebar last frame. */
static int titlebar_menu_at(const App* a, int mx, int my)
{
    for (int i = 0; i < 4; ++i) {
        const SDL_Rect* r = &a->menu_rects[i];
        if (r->w <= 0) continue;
        if (mx >= r->x && mx < r->x + r->w &&
            my >= r->y && my < r->y + r->h) return i;
    }
    return -1;
}

static bool window_is_maximized(App* a)
{
    return (SDL_GetWindowFlags(a->window) & SDL_WINDOW_MAXIMIZED) != 0;
}

static void titlebar_button_invoke(App* a, int btn)
{
    switch (btn) {
        case TBB_MIN:
#if defined(_WIN32)
        {
            /* SDL_MinimizeWindow on a borderless+thickframe window can be
             * inconsistent — go through Win32 directly. */
            SDL_SysWMinfo wmi; SDL_VERSION(&wmi.version);
            if (SDL_GetWindowWMInfo(a->window, &wmi))
                ShowWindow(wmi.info.win.window, SW_MINIMIZE);
            else SDL_MinimizeWindow(a->window);
        }
#else
            SDL_MinimizeWindow(a->window);
#endif
            break;
        case TBB_MAX:
#if defined(_WIN32)
        {
            SDL_SysWMinfo wmi; SDL_VERSION(&wmi.version);
            if (SDL_GetWindowWMInfo(a->window, &wmi)) {
                HWND hwnd = wmi.info.win.window;
                ShowWindow(hwnd,
                    window_is_maximized(a) ? SW_RESTORE : SW_MAXIMIZE);
            } else {
                if (window_is_maximized(a)) SDL_RestoreWindow(a->window);
                else                        SDL_MaximizeWindow(a->window);
            }
        }
#else
            if (window_is_maximized(a)) SDL_RestoreWindow(a->window);
            else                        SDL_MaximizeWindow(a->window);
#endif
            break;
        case TBB_CLOSE:
            if (confirm_discard(a)) a->running = false;
            break;
        default: break;
    }
}

static void render_titlebar(App* a, int TBH)
{
    int right = a->win_w;
    int text_h = font_ascent(a->font_body) + font_descent(a->font_body);
    int by = (TBH - text_h) / 2 + font_ascent(a->font_body);

    /* App icon (small folder glyph) at the very left. */
    int icon_pad = 8;
    int icon_sz  = TBH - 2 * icon_pad;
    icon_draw(a->renderer, ICON_FOLDER_OPEN,
              icon_pad, icon_pad, icon_sz, a->fg_link);
    int x = icon_pad + icon_sz + 10;

    /* Menu items: File / Edit / View / Help. */
    for (int i = 0; i < 4; ++i) {
        const char* label = MENU_LABELS[i];
        int lw = font_measure(a->font_body, label, strlen(label));
        int item_w = lw + 16;
        SDL_Rect r = { x, 2, item_w, TBH - 4 };
        a->menu_rects[i] = r;
        float t  = a->menu_hover_t[i];
        float et = ease_out_cubic(t);
        bool is_open = (a->menu_open == i);
        if (is_open || t > 0.005f) {
            SDL_SetRenderDrawColor(a->renderer,
                a->bg_sidebar_hover.r, a->bg_sidebar_hover.g,
                a->bg_sidebar_hover.b,
                (Uint8)(is_open ? 220 : 160 * et));
            fill_rrect(a->renderer, r, 4);
        }
        SDL_Color tc = (is_open || t > 0.05f) ? a->fg_link : a->fg;
        font_draw_line(a->font_body, label, strlen(label),
                       x + 8, by, tc);
        x += item_w + 2;
    }

    /* Filename text in the center-ish (drag area shows the doc name). */
    {
        const char* title = (a->fm_present && a->fm_title[0])
                            ? a->fm_title
                            : (a->note_path ? vault_basename(a->note_path) : "Downsee");
        int tw = font_measure(a->font_body, title, strlen(title));
        int min_x = x + 16;
        int max_x = right - 3 * TB_BTN_W - 16;
        int tx = (a->win_w - tw) / 2;
        if (tx < min_x) tx = min_x;
        if (tx + tw > max_x) tx = max_x - tw;
        if (tx >= min_x && tx + tw <= max_x) {
            font_draw_line(a->font_body, title, strlen(title),
                           tx, by, a->fg_muted);
        }
    }

    /* Window controls (right edge): minimize / maximize / close.
     * Hover: bg fill (red for close); icon stays clear. */
    static const IconId TBB_ICONS[3] = {
        ICON_WIN_MIN, ICON_WIN_MAX, ICON_WIN_CLOSE
    };
    int btn_y = 0;
    int btn_h = TBH;
    for (int b = 0; b < 3; ++b) {
        int bx = right - (3 - b) * TB_BTN_W;
        float t = a->tb_btn_hover_t[b];
        float et = ease_out_cubic(t);
        if (et > 0.005f) {
            SDL_Rect r = { bx, btn_y, TB_BTN_W, btn_h };
            SDL_Color hc = (b == TBB_CLOSE)
                ? (SDL_Color){ 232, 17, 35, 255 }    /* Win-style close-red */
                : a->bg_sidebar_hover;
            SDL_SetRenderDrawColor(a->renderer, hc.r, hc.g, hc.b,
                                   (Uint8)(255 * et));
            SDL_RenderFillRect(a->renderer, &r);
        }
        IconId id = TBB_ICONS[b];
        if (b == TBB_MAX && window_is_maximized(a)) id = ICON_WIN_RESTORE;
        int isz = 12;
        int ix = bx + (TB_BTN_W - isz) / 2;
        int iy = (btn_h - isz) / 2;
        SDL_Color ic = (b == TBB_CLOSE && et > 0.05f)
            ? (SDL_Color){ 250, 250, 250, 255 } : a->fg;
        icon_draw(a->renderer, id, ix, iy, isz, ic);
    }
}

#if defined(_WIN32)
static LRESULT CALLBACK downsee_wndproc(HWND hwnd, UINT msg,
                                        WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_NCCALCSIZE:
            /* wp == TRUE: lp points to NCCALCSIZE_PARAMS; returning 0
             * makes the proposed client rect == proposed window rect (no
             * non-client area carved out). This is the well-known "custom
             * frame" trick paired with WS_THICKFRAME. */
            if (wp == TRUE) return 0;
            break;
        case WM_GETMINMAXINFO: {
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { 0 };
            mi.cbSize = sizeof mi;
            if (mon && GetMonitorInfoA(mon, &mi)) {
                MINMAXINFO* m = (MINMAXINFO*)lp;
                int mx = mi.rcMonitor.left;
                int my = mi.rcMonitor.top;
                m->ptMaxPosition.x = mi.rcWork.left   - mx;
                m->ptMaxPosition.y = mi.rcWork.top    - my;
                m->ptMaxSize.x     = mi.rcWork.right  - mi.rcWork.left;
                m->ptMaxSize.y     = mi.rcWork.bottom - mi.rcWork.top;
                m->ptMaxTrackSize.x = m->ptMaxSize.x;
                m->ptMaxTrackSize.y = m->ptMaxSize.y;
            }
            return 0;
        }
    }
    return CallWindowProc(g_orig_wndproc, hwnd, msg, wp, lp);
}
#endif

/* SDL_HitTest callback: tells Windows which areas of our borderless
 * window are drag area, which are resize edges. Aero snap and Win+arrow
 * shortcuts work because Windows treats SDL_HITTEST_DRAGGABLE as
 * HTCAPTION. Returning HITTEST_NORMAL lets the click reach our normal
 * SDL_MOUSEBUTTONDOWN handler (used for menu items + window controls). */
static SDL_HitTestResult SDLCALL window_hit_test_cb(SDL_Window* w,
    const SDL_Point* p, void* data)
{
    (void)w;
    App* a = (App*)data;
    int W = a->win_w, H = a->win_h;
    int RZ = 5;     /* resize-edge thickness */

    if (!window_is_maximized(a)) {
        if (p->x < RZ        && p->y < RZ)        return SDL_HITTEST_RESIZE_TOPLEFT;
        if (p->x >= W - RZ   && p->y < RZ)        return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (p->x < RZ        && p->y >= H - RZ)   return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (p->x >= W - RZ   && p->y >= H - RZ)   return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (p->y < RZ)                            return SDL_HITTEST_RESIZE_TOP;
        if (p->y >= H - RZ)                       return SDL_HITTEST_RESIZE_BOTTOM;
        if (p->x < RZ)                            return SDL_HITTEST_RESIZE_LEFT;
        if (p->x >= W - RZ)                       return SDL_HITTEST_RESIZE_RIGHT;
    }

    if (p->y < title_bar_h(a)) {
        if (titlebar_button_at(a, p->x, p->y) != TBB_NONE) return SDL_HITTEST_NORMAL;
        if (titlebar_menu_at(a, p->x, p->y) != -1)         return SDL_HITTEST_NORMAL;
        return SDL_HITTEST_DRAGGABLE;
    }

    return SDL_HITTEST_NORMAL;
}

/* Top-bar buttons. CB_NONE means "not on a button". The order here also
 * fixes the right-to-left layout in render_chrome (rightmost first). */
enum ChromeButton {
    CB_NONE      = -1,
    CB_SETTINGS  = 0,    /* gear icon — opens settings overlay */
    CB_MODE      = 1,    /* PREVIEW / EDIT pill */
    CB_OUTLINE   = 2,    /* toggle outline panel */
    CB_SIDEBAR   = 3,    /* toggle sidebar */
    CB_FIND      = 4,    /* in-doc find */
    CB_VSEARCH   = 5,    /* vault-wide search */
    CB_CMDP      = 6,    /* command palette */
};
#define CB_COUNT 7

static int chrome_button_size(const App* a) {
    return chrome_row_h(a);     /* square buttons matching the chrome ROW height */
}

/* Forward decls for actions used by chrome buttons. They're defined far
 * later in the file, so we promise their existence here. */
static void action_settings      (App* a);
static void action_toggle_sidebar(App* a);
static void action_toggle_edit   (App* a);
static void action_find          (App* a);
static void action_outline_pin   (App* a);
static void action_vsearch       (App* a);
static void search_close         (App* a);
static void cmdp_open            (App* a);

/* Hit-test the chrome bar. Returns one of the CB_* enum values, or
 * CB_NONE. The buttons live in the right edge of the chrome bar; the
 * leftmost area is the breadcrumb (vault › note title), not clickable. */
static int chrome_hit_test(const App* a, int mx, int my)
{
    /* Title bar area is its own zone (drag + window controls), not part
     * of chrome buttons. */
    if (my < title_bar_h(a) || my >= chrome_bar_h(a)) return CB_NONE;
    int sz = chrome_button_size(a);
    int right = a->win_w;
    /* Settings (rightmost). */
    if (mx >= right - sz       && mx < right) return CB_SETTINGS;
    if (mx >= right - 2 * sz   && mx < right - sz)     return CB_OUTLINE;
    if (mx >= right - 3 * sz   && mx < right - 2 * sz) return CB_SIDEBAR;
    if (mx >= right - 4 * sz   && mx < right - 3 * sz) return CB_FIND;
    if (mx >= right - 5 * sz   && mx < right - 4 * sz) return CB_VSEARCH;
    if (mx >= right - 6 * sz   && mx < right - 5 * sz) return CB_CMDP;
    int pill_w = 100;
    int pill_x = right - 6 * sz - 10 - pill_w;
    if (mx >= pill_x && mx < pill_x + pill_w) return CB_MODE;
    return CB_NONE;
}

static void chrome_button_invoke(App* a, int btn)
{
    switch (btn) {
        case CB_SETTINGS: action_settings(a);       break;
        case CB_OUTLINE:  action_outline_pin(a);    break;
        case CB_SIDEBAR:  action_toggle_sidebar(a); break;
        case CB_FIND:
            if (a->search_mode != 0) search_close(a);
            else                      action_find(a);
            break;
        case CB_VSEARCH:  action_vsearch(a);        break;
        case CB_CMDP:     cmdp_open(a);             break;
        case CB_MODE:     action_toggle_edit(a);    break;
        default: break;
    }
}

/* Forward decls for the title-bar render. */
static void render_titlebar(App* a, int TBH);

/* Render the top chrome bar: bg slab, title bar at the top with menu items
 * + window controls, then the tool row (breadcrumb, mode pill, icons). */
static void render_chrome(App* a)
{
    int H   = chrome_bar_h(a);
    int TBH = title_bar_h(a);
    int CRH = chrome_row_h(a);

    SDL_Rect bg = { 0, 0, a->win_w, H };
    SDL_SetRenderDrawColor(a->renderer,
        a->bg_status.r, a->bg_status.g, a->bg_status.b, 255);
    SDL_RenderFillRect(a->renderer, &bg);

    /* Hairline divider between title bar and tool row. */
    SDL_Rect tbdiv = { 0, TBH, a->win_w, 1 };
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 40);
    SDL_RenderFillRect(a->renderer, &tbdiv);

    /* Hairline divider below the chrome. */
    SDL_Rect div = { 0, H, a->win_w, 1 };
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 70);
    SDL_RenderFillRect(a->renderer, &div);

    /* Title bar contents (menu items, drag area, min/max/close). */
    render_titlebar(a, TBH);

    /* Breadcrumb: vault › title. Each segment is its own clickable hit-rect
     * with an animated hover color cross-fade. Vertical centering is within
     * the chrome ROW (below the title bar), not the full chrome height. */
    int text_h = font_ascent(a->font_body) + font_descent(a->font_body);
    int by = TBH + (CRH - text_h) / 2 + font_ascent(a->font_body);
    int bx = 14;
    a->crumb_rect_vault = (SDL_Rect){ 0, 0, 0, 0 };
    a->crumb_rect_title = (SDL_Rect){ 0, 0, 0, 0 };
    /* Cross-fade helper: returns a color mixed from base toward fg_link by t. */
    #define CRUMB_COLOR(base_, t_)                                          \
        ({                                                                  \
            float _et = ease_out_cubic(t_);                                 \
            int _r = (int)((base_).r + (a->fg_link.r - (base_).r) * _et);   \
            int _g = (int)((base_).g + (a->fg_link.g - (base_).g) * _et);   \
            int _b = (int)((base_).b + (a->fg_link.b - (base_).b) * _et);   \
            (SDL_Color){ (Uint8)_r, (Uint8)_g, (Uint8)_b, 255 };            \
        })
    if (a->vault.dir) {
        const char* vault = vault_basename(a->vault.dir);
        int vw = font_measure(a->font_body, vault, strlen(vault));
        a->crumb_rect_vault = (SDL_Rect){ bx - 4, TBH + 4, vw + 8, CRH - 8 };
        SDL_Color vc = CRUMB_COLOR(a->fg_muted, a->crumb_hover_t[0]);
        if (a->crumb_hover_t[0] > 0.01f) {
            SDL_SetRenderDrawColor(a->renderer,
                a->bg_sidebar_hover.r, a->bg_sidebar_hover.g,
                a->bg_sidebar_hover.b,
                (Uint8)(160 * ease_out_cubic(a->crumb_hover_t[0])));
            fill_rrect(a->renderer, a->crumb_rect_vault, 4);
        }
        font_draw_line(a->font_body, vault, strlen(vault), bx, by, vc);
        bx += vw + 6;
        font_draw_line(a->font_body, "\xe2\x80\xba", 3,    /* › */
                       bx, by, a->fg_muted);
        bx += font_measure(a->font_body, "\xe2\x80\xba", 3) + 6;
    }
    const char* title = (a->fm_present && a->fm_title[0])
                       ? a->fm_title
                       : (a->note_path ? vault_basename(a->note_path) : "(unsaved)");
    int tw = font_measure(a->font_body, title, strlen(title));
    a->crumb_rect_title = (SDL_Rect){ bx - 4, TBH + 4, tw + 8, CRH - 8 };
    SDL_Color tc = CRUMB_COLOR(a->fg, a->crumb_hover_t[1]);
    if (a->crumb_hover_t[1] > 0.01f) {
        SDL_SetRenderDrawColor(a->renderer,
            a->bg_sidebar_hover.r, a->bg_sidebar_hover.g,
            a->bg_sidebar_hover.b,
            (Uint8)(160 * ease_out_cubic(a->crumb_hover_t[1])));
        fill_rrect(a->renderer, a->crumb_rect_title, 4);
    }
    font_draw_line(a->font_body, title, strlen(title), bx, by, tc);
    if (a->buf.dirty) {
        bx += tw + 6;
        font_draw_line(a->font_body, "\xe2\x80\xa2", 3,    /* • */
                       bx, by, a->fg_link);
    }
    #undef CRUMB_COLOR

    /* Right-side buttons. */
    int sz    = chrome_button_size(a);
    int right = a->win_w;

    /* Animated chrome cell. Coords are inside the chrome row [TBH..H], so
     * cell rects use TBH as their origin. */
    #define BTN_PREP(x_, btn_id_, active_)                                    \
        ({                                                                    \
            float _t   = a->chrome_hover_t[btn_id_];                          \
            float _et  = ease_out_cubic(_t);                                  \
            float _pr  = a->chrome_press_t[btn_id_];                          \
            int   _bx  = (x_);                                                \
            if (_t > 0.005f) {                                                \
                SDL_Rect _r = { _bx + 4, TBH + 6, sz - 8, CRH - 12 };         \
                SDL_SetRenderDrawColor(a->renderer,                           \
                    a->bg_sidebar_hover.r, a->bg_sidebar_hover.g,             \
                    a->bg_sidebar_hover.b,                                    \
                    (Uint8)(190 * _et + 60 * _pr));                           \
                fill_rrect(a->renderer, _r, 5);                               \
            }                                                                 \
            if (_t > 0.005f) {                                                \
                int _max_w = sz - 16;                                         \
                int _uw    = (int)(_max_w * _et);                             \
                if (_uw < 2) _uw = 2;                                         \
                SDL_Rect _u = { _bx + (sz - _uw) / 2, H - 4, _uw, 2 };        \
                SDL_SetRenderDrawColor(a->renderer,                           \
                    a->fg_link.r, a->fg_link.g, a->fg_link.b,                 \
                    (Uint8)(220 * _et));                                      \
                SDL_RenderFillRect(a->renderer, &_u);                         \
            }                                                                 \
            int _ar = (int)(a->fg_muted.r + (a->fg_link.r - a->fg_muted.r) * _et);\
            int _ag = (int)(a->fg_muted.g + (a->fg_link.g - a->fg_muted.g) * _et);\
            int _ab = (int)(a->fg_muted.b + (a->fg_link.b - a->fg_muted.b) * _et);\
            (active_) ? a->fg_link                                            \
                      : (SDL_Color){ (Uint8)_ar, (Uint8)_ag, (Uint8)_ab, 255 };\
        })

    int ipad     = 9;
    int icon_sz  = sz - 2 * ipad;
    int icon_off_y = TBH + ipad;

    /* Settings (rightmost): sliders icon. */
    {
        SDL_Color gc = BTN_PREP(right - sz, CB_SETTINGS, false);
        icon_draw(a->renderer, ICON_SETTINGS,
                  right - sz + ipad, icon_off_y, icon_sz, gc);
    }
    /* Outline pin toggle: list icon, accented when pinned. */
    {
        SDL_Color gc = BTN_PREP(right - 2 * sz, CB_OUTLINE, a->outline_pinned);
        icon_draw(a->renderer, ICON_OUTLINE,
                  right - 2 * sz + ipad, icon_off_y, icon_sz, gc);
    }
    /* Sidebar toggle: panel icon, fill on left half when sidebar is open. */
    {
        SDL_Color gc = BTN_PREP(right - 3 * sz, CB_SIDEBAR, false);
        icon_draw(a->renderer,
                  a->sidebar_open ? ICON_SIDEBAR_OPEN : ICON_SIDEBAR_CLOSED,
                  right - 3 * sz + ipad, icon_off_y, icon_sz, gc);
    }
    /* Find: magnifier (in-doc search). */
    {
        SDL_Color gc = BTN_PREP(right - 4 * sz, CB_FIND, false);
        icon_draw(a->renderer, ICON_FIND,
                  right - 4 * sz + ipad, icon_off_y, icon_sz, gc);
    }
    /* Vault search: text-search icon (search across all notes). */
    {
        SDL_Color gc = BTN_PREP(right - 5 * sz, CB_VSEARCH, false);
        icon_draw(a->renderer, ICON_VAULT_SEARCH,
                  right - 5 * sz + ipad, icon_off_y, icon_sz, gc);
    }
    /* Command palette: terminal prompt icon, sits between vault search
     * and the mode pill. */
    {
        SDL_Color gc = BTN_PREP(right - 6 * sz, CB_CMDP, false);
        icon_draw(a->renderer, ICON_COMMAND,
                  right - 6 * sz + ipad, icon_off_y, icon_sz, gc);
    }
    #undef BTN_PREP

    /* Mode pill: PREVIEW / EDIT. Lives in the chrome row only. */
    {
        int pill_w = 100;
        int pill_h = CRH - 12;
        int pill_x = right - 6 * sz - 10 - pill_w;
        int pill_y = TBH + (CRH - pill_h) / 2;
        float t  = a->chrome_hover_t[CB_MODE];
        float et = ease_out_cubic(t);
        float pr = a->chrome_press_t[CB_MODE];
        bool active = a->edit_mode;

        SDL_Color fill;
        if (active) {
            fill = a->fg_link;
        } else {
            /* Idle: same tone as chrome bar, very subtly lifted on hover. */
            int br = (int)(a->bg_sidebar_active.r +
                          (a->bg_sidebar_hover.r - a->bg_sidebar_active.r) * et);
            int bg = (int)(a->bg_sidebar_active.g +
                          (a->bg_sidebar_hover.g - a->bg_sidebar_active.g) * et);
            int bb = (int)(a->bg_sidebar_active.b +
                          (a->bg_sidebar_hover.b - a->bg_sidebar_active.b) * et);
            fill = (SDL_Color){ (Uint8)br, (Uint8)bg, (Uint8)bb, 255 };
        }
        SDL_Color pill_c = fill;
        pill_c.a = (Uint8)(active ? 255 : (200 + 30 * pr));
        pill_draw(a->renderer, pill_x, pill_y, pill_w, pill_h,
                  pill_h / 2, pill_c);

        const char* label = active ? "EDIT" : "PREVIEW";
        int lw = font_measure(a->font_body, label, strlen(label));
        SDL_Color lc;
        if (active) {
            int lum = a->fg_link.r * 30 + a->fg_link.g * 59 + a->fg_link.b * 11;
            lc = (lum > 12000) ? (SDL_Color){ 20, 20, 26, 255 }
                               : (SDL_Color){ 240, 240, 250, 255 };
        } else {
            int lr = (int)(a->fg.r + (a->fg_link.r - a->fg.r) * et);
            int lg = (int)(a->fg.g + (a->fg_link.g - a->fg.g) * et);
            int lb = (int)(a->fg.b + (a->fg_link.b - a->fg.b) * et);
            lc = (SDL_Color){ (Uint8)lr, (Uint8)lg, (Uint8)lb, 255 };
        }
        font_draw_line(a->font_body, label, strlen(label),
                       pill_x + (pill_w - lw) / 2, by, lc);
    }
}

/* Count files (non-dirs) that live directly inside the folder at vault
 * index `folder_idx`. Items are children iff their path starts with the
 * folder path + slash AND no further slash appears beyond the prefix. */
static int sidebar_folder_file_count(const App* a, int folder_idx)
{
    if (folder_idx < 0 || folder_idx >= (int)a->vault.count) return 0;
    const VaultItem* f = &a->vault.items[folder_idx];
    if (!f->is_dir) return 0;
    size_t plen = strlen(f->path);
    int count = 0;
    for (size_t i = 0; i < a->vault.count; ++i) {
        if ((int)i == folder_idx) continue;
        const VaultItem* it = &a->vault.items[i];
        if (it->is_dir) continue;
        if (strncmp(it->path, f->path, plen) != 0) continue;
        if (it->path[plen] != '/' && it->path[plen] != '\\') continue;
        /* Direct child only — no further '/' in the suffix. */
        const char* sub = it->path + plen + 1;
        if (strchr(sub, '/') != NULL || strchr(sub, '\\') != NULL) continue;
        count++;
    }
    return count;
}

static int sidebar_item_height(const App* a) { return font_line_height(a->font_body) + 10; }
/* Sidebar items start below the chrome bar + a vault-name header (a body
 * line + 16px of breathing room). */
static int sidebar_items_top  (const App* a)
{
    return chrome_bar_h(a) + font_line_height(a->font_body) + 18;
}

/* Build the list of vault item indices currently visible (filter out items
 * inside collapsed folders). Stored on App so the click handler can map a
 * row back to a vault item. Called by render_sidebar; cheap O(N). */
static void sidebar_compute_visible(App* a)
{
    if (a->sidebar_visible_cap < (int)a->vault.count) {
        a->sidebar_visible_cap = (int)a->vault.count + 8;
        a->sidebar_visible = realloc(a->sidebar_visible,
                                     a->sidebar_visible_cap * sizeof(int));
    }
    a->sidebar_visible_count = 0;

    char skip_prefix[1024];
    size_t skip_len = 0;
    for (size_t i = 0; i < a->vault.count; ++i) {
        VaultItem* it = &a->vault.items[i];
        if (skip_len > 0 && strncmp(it->path, skip_prefix, skip_len) == 0) continue;
        skip_len = 0;

        a->sidebar_visible[a->sidebar_visible_count++] = (int)i;

        if (it->is_dir && it->collapsed) {
            int n = snprintf(skip_prefix, sizeof skip_prefix, "%s/", it->path);
            skip_len = (n > 0 && n < (int)sizeof skip_prefix) ? (size_t)n : 0;
        }
    }
}

static int sidebar_item_at(const App* a, int mx, int my)
{
    if (!a->sidebar_open) return -1;
    if (mx < 0 || mx >= a->sidebar_w) return -1;
    int top = sidebar_items_top(a);
    if (my < top) return -1;
    int row = (my - top + a->sidebar_scroll_y) / sidebar_item_height(a);
    if (row < 0 || row >= a->sidebar_visible_count) return -1;
    return a->sidebar_visible[row];   /* return vault.items index */
}

static int sidebar_max_scroll(const App* a)
{
    int item_h  = sidebar_item_height(a);
    int items_h = a->sidebar_visible_count * item_h;
    int avail   = a->win_h - sidebar_items_top(a) - status_bar_h(a);
    int over    = items_h - avail;
    return over > 0 ? over : 0;
}

/* Does the ancestor of `row` at depth `a` have more children visible
 * after `row`? Used to decide whether to draw a continuing tree-guide
 * vertical line at depth a's column for each row below. */
static bool sidebar_ancestor_continues(const App* a, int row, int anc_depth)
{
    for (int j = row + 1; j < a->sidebar_visible_count; ++j) {
        int d = a->vault.items[a->sidebar_visible[j]].depth;
        if (d <  anc_depth) return false;     /* left the subtree */
        if (d == anc_depth) return true;       /* found another sibling */
    }
    return false;
}

#define SIDEBAR_INDENT_STEP 16
#define SIDEBAR_ICON_SZ     16

static void render_sidebar(App* a)
{
    if (!a->sidebar_open) return;

    sidebar_compute_visible(a);

    SDL_Rect bg = { 0, chrome_bar_h(a), a->sidebar_w,
                    a->win_h - chrome_bar_h(a) };
    SDL_SetRenderDrawColor(a->renderer,
        a->bg_sidebar.r, a->bg_sidebar.g, a->bg_sidebar.b, a->bg_sidebar.a);
    SDL_RenderFillRect(a->renderer, &bg);

    /* Vertical divider between sidebar and document area, blended into the
     * muted color so it reads as a hairline rather than a slab. */
    SDL_Rect divider = { a->sidebar_w - 1, chrome_bar_h(a),
                         1, a->win_h - chrome_bar_h(a) };
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 60);
    SDL_RenderFillRect(a->renderer, &divider);

    /* Vault name header — folder icon + name. The icon disambiguates the
     * header from the items below (otherwise it just looks like row #1 with
     * no caret). */
    int header_y = chrome_bar_h(a) + 10;
    const char* title = a->vault.dir ? vault_basename(a->vault.dir) : "vault";
    int hdr_icon_sz = SIDEBAR_ICON_SZ + 2;
    int hdr_icon_y  = header_y + 1;
    icon_draw(a->renderer, ICON_FOLDER_OPEN,
              SIDEBAR_PAD_X, hdr_icon_y, hdr_icon_sz, a->fg);
    font_draw_line(a->font_body, title, strlen(title),
                   SIDEBAR_PAD_X + hdr_icon_sz + 8,
                   header_y + font_ascent(a->font_body) + 2, a->fg);
    /* Hairline separator under the header so the items section reads as
     * its own region. */
    SDL_Rect hdr_sep = { SIDEBAR_PAD_X, header_y + sidebar_item_height(a) - 4,
                         a->sidebar_w - 2 * SIDEBAR_PAD_X, 1 };
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 40);
    SDL_RenderFillRect(a->renderer, &hdr_sep);

    SDL_Rect clip = {
        0, sidebar_items_top(a),
        a->sidebar_w - 1, a->win_h - sidebar_items_top(a) - status_bar_h(a)
    };
    SDL_RenderSetClipRect(a->renderer, &clip);

    int item_h = sidebar_item_height(a);
    int item_y = sidebar_items_top(a) - a->sidebar_scroll_y;

    /* One-shot diagnostic: log what the sidebar sees on first draw. If
     * users report "I only see the header", they can paste this line. */
    static int diag_logged = 0;
    if (!diag_logged) {
        diag_logged = 1;
        fprintf(stderr,
            "[sidebar] vault.dir=%s count=%zu visible=%d sidebar_w=%d "
            "items_top=%d scroll_y=%d item_h=%d\n",
            a->vault.dir ? a->vault.dir : "(null)",
            a->vault.count, a->sidebar_visible_count,
            a->sidebar_w, sidebar_items_top(a),
            a->sidebar_scroll_y, item_h);
    }

    /* Empty-state placeholder so a blank sidebar isn't mysterious. */
    if (a->sidebar_visible_count == 0) {
        const char* msg = (a->vault.count == 0)
            ? "(no .md files in vault)"
            : "(no items visible)";
        font_draw_line(a->font_body, msg, strlen(msg),
                       SIDEBAR_PAD_X,
                       item_y + font_ascent(a->font_body) + 5,
                       a->fg_muted);
    }

    /* Color for tree guide lines — very faint muted. */
    SDL_Color guide_c = a->fg_muted;
    guide_c.a = 50;

    for (int row = 0; row < a->sidebar_visible_count; ++row) {
        int vi = a->sidebar_visible[row];
        VaultItem* it = &a->vault.items[vi];
        bool is_sel = vi == a->vault.selected;
        bool is_hov = vi == a->sidebar_hover;
        int  depth  = it->depth;

        /* Selection/hover bg comes first so the tree lines + icons paint
         * on top with the correct contrast. */
        if (is_sel) {
            SDL_Rect tint = { 0, item_y, a->sidebar_w - 1, item_h };
            SDL_SetRenderDrawColor(a->renderer,
                a->bg_sidebar_hover.r, a->bg_sidebar_hover.g,
                a->bg_sidebar_hover.b, 200);
            SDL_RenderFillRect(a->renderer, &tint);
            SDL_Rect bar = { 0, item_y, 3, item_h };
            SDL_SetRenderDrawColor(a->renderer,
                a->fg_link.r, a->fg_link.g, a->fg_link.b, 255);
            SDL_RenderFillRect(a->renderer, &bar);
        } else if (is_hov) {
            SDL_Rect r = { 0, item_y, a->sidebar_w - 1, item_h };
            SDL_SetRenderDrawColor(a->renderer,
                a->bg_sidebar_hover.r, a->bg_sidebar_hover.g,
                a->bg_sidebar_hover.b, 130);
            SDL_RenderFillRect(a->renderer, &r);
        }

        /* Tree guide lines: for each ancestor depth a in [0..depth-1],
         * draw a thin vertical line at that depth's column if the ancestor
         * has more children below this row. The "continuing" lines extend
         * the full row height; the "last child" lines stop at row mid. */
        SDL_SetRenderDrawColor(a->renderer,
            guide_c.r, guide_c.g, guide_c.b, guide_c.a);
        for (int d = 0; d < depth; ++d) {
            int gx = SIDEBAR_PAD_X + d * SIDEBAR_INDENT_STEP + 8;
            bool continues = sidebar_ancestor_continues(a, row, d + 1);
            int top = item_y;
            int bot = (d == depth - 1 && !continues)
                      ? item_y + item_h / 2
                      : item_y + item_h;
            SDL_RenderDrawLine(a->renderer, gx, top, gx, bot);
            /* L-stub: short horizontal connecting the vertical line to the
             * icon column, drawn only at the immediate-parent column. */
            if (d == depth - 1) {
                int hx = gx + 1;
                int hy = item_y + item_h / 2;
                int hr = SIDEBAR_PAD_X + depth * SIDEBAR_INDENT_STEP - 2;
                SDL_RenderDrawLine(a->renderer, hx, hy, hr, hy);
            }
        }

        /* Layout: caret (folders only) → icon → text. */
        int x_indent = SIDEBAR_PAD_X + depth * SIDEBAR_INDENT_STEP;
        int icon_y   = item_y + (item_h - SIDEBAR_ICON_SZ) / 2;
        int x_caret  = x_indent;
        int x_icon   = x_indent + 14;
        int x_text   = x_icon + SIDEBAR_ICON_SZ + 6;
        int baseline = item_y + font_ascent(a->font_body) + 5;

        SDL_Color icon_c = is_sel ? a->fg_link : a->fg_muted;

        if (it->is_dir) {
            icon_draw(a->renderer,
                      it->collapsed ? ICON_CARET_RIGHT : ICON_CARET_DOWN,
                      x_caret, icon_y, SIDEBAR_ICON_SZ, a->fg_muted);
            icon_draw(a->renderer,
                      it->collapsed ? ICON_FOLDER : ICON_FOLDER_OPEN,
                      x_icon, icon_y, SIDEBAR_ICON_SZ, icon_c);
        } else {
            icon_draw(a->renderer, ICON_FILE,
                      x_icon, icon_y, SIDEBAR_ICON_SZ, icon_c);
        }

        SDL_Color text_c = is_sel ? a->fg
                         : it->is_dir ? a->fg : a->fg;
        font_draw_line(a->font_body, it->name, strlen(it->name),
                       x_text, baseline, text_c);

        /* Right-edge widget: dirty marker / folder count. */
        if (it->is_dir) {
            int count = sidebar_folder_file_count(a, vi);
            if (count > 0) {
                char cnt[16];
                snprintf(cnt, sizeof cnt, "%d", count);
                int cw = font_measure(a->font_body, cnt, strlen(cnt));
                font_draw_line(a->font_body, cnt, strlen(cnt),
                               a->sidebar_w - SIDEBAR_PAD_X - cw,
                               baseline, a->fg_muted);
            }
        } else if (is_sel && a->buf.dirty) {
            const char* dot = "\xe2\x80\xa2";    /* • */
            int dw = font_measure(a->font_body, dot, 3);
            font_draw_line(a->font_body, dot, 3,
                           a->sidebar_w - SIDEBAR_PAD_X - dw,
                           baseline, a->fg_link);
        }
        item_y += item_h;
    }
    SDL_RenderSetClipRect(a->renderer, NULL);
}

/* Step-arrow buttons live at the top and bottom of every scrollbar track.
 * They occupy SB_ARROW_H pixels each, shrinking the thumb travel area to
 * an "inner track". When the track is too short, arrows are suppressed and
 * the inner track equals the full track. */
#define SB_ARROW_H 12

static bool sb_track_has_arrows(const SDL_Rect* track)
{
    /* Need room for arrows + at least min thumb (24) + a few px breathing. */
    return track->h >= 2 * SB_ARROW_H + 28;
}

static void sb_arrow_rects(const SDL_Rect* track, SDL_Rect* up, SDL_Rect* dn)
{
    if (up) *up = (SDL_Rect){ track->x, track->y, track->w, SB_ARROW_H };
    if (dn) *dn = (SDL_Rect){ track->x, track->y + track->h - SB_ARROW_H,
                              track->w, SB_ARROW_H };
}

static void sb_inner_track(const SDL_Rect* track, SDL_Rect* inner)
{
    if (sb_track_has_arrows(track)) {
        *inner = (SDL_Rect){ track->x, track->y + SB_ARROW_H,
                             track->w, track->h - 2 * SB_ARROW_H };
    } else {
        *inner = *track;
    }
}

/* Compute the document scrollbar geometry (track + thumb). Returns 1 if
 * the scrollbar should be drawn / be hit-testable, 0 if content fits and
 * it's hidden. */
static int doc_scrollbar_geom(const App* a,
                              SDL_Rect* track_out, SDL_Rect* thumb_out)
{
    int total = a->doc_height_px;
    int vh    = viewport_h(a);
    if (total <= vh) return 0;
    int track_w = 10;
    int track_x = doc_x_right(a) - track_w;
    int track_y = doc_y_top(a);
    SDL_Rect track = { track_x, track_y, track_w, vh };
    SDL_Rect inner; sb_inner_track(&track, &inner);
    int thumb_h = inner.h * vh / total;
    if (thumb_h < 24) thumb_h = 24;
    if (thumb_h > inner.h - 4) thumb_h = inner.h - 4;
    if (thumb_h < 12) thumb_h = inner.h;     /* degenerate; renders flat */
    int max_y   = inner.h - thumb_h;
    int max_sc  = max_scroll(a);
    int thumb_y = (max_sc > 0 && max_y > 0)
                  ? (max_y * a->scroll_y / max_sc) : 0;
    if (track_out) *track_out = track;
    if (thumb_out) *thumb_out = (SDL_Rect){ track_x + 2, inner.y + thumb_y,
                                            track_w - 4, thumb_h };
    return 1;
}

/* Draws a small filled triangle (point=up if up==true) inside cell, built
 * up from 1px-tall horizontal strips so we don't need an external triangle
 * primitive. */
static void sb_draw_arrow_glyph(App* a, const SDL_Rect* cell, bool up,
                                int alpha)
{
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, alpha);
    int pad_y = 3, pad_x = 2;
    int gh = cell->h - 2 * pad_y;     if (gh < 3) gh = 3;
    int gw = cell->w - 2 * pad_x;     if (gw < 3) gw = 3;
    int gx = cell->x + (cell->w - gw) / 2;
    int gy = cell->y + (cell->h - gh) / 2;
    int denom = gh > 1 ? gh - 1 : 1;
    for (int row = 0; row < gh; ++row) {
        int idx = up ? row : (gh - 1 - row);
        int w   = 1 + (gw - 1) * idx / denom;
        SDL_Rect r = { gx + (gw - w) / 2, gy + row, w, 1 };
        SDL_RenderFillRect(a->renderer, &r);
    }
}

static void sb_draw_arrows(App* a, const SDL_Rect* track, int alpha)
{
    if (!sb_track_has_arrows(track)) return;
    SDL_Rect up, dn;
    sb_arrow_rects(track, &up, &dn);
    sb_draw_arrow_glyph(a, &up, true,  alpha);
    sb_draw_arrow_glyph(a, &dn, false, alpha);
}

static void render_scrollbar(App* a)
{
    SDL_Rect track, thumb;
    if (!doc_scrollbar_geom(a, &track, &thumb)) return;
    /* Track: faint band so the thumb has a visible groove to live in. */
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 30);
    SDL_RenderFillRect(a->renderer, &track);
    /* Thumb: brighter while being dragged. */
    int alpha = (a->sb_drag == SB_DOC) ? 220 : 150;
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, alpha);
    SDL_RenderFillRect(a->renderer, &thumb);
    sb_draw_arrows(a, &track, alpha);
}

/* ---- generic overlay-list scrollbar plumbing -------------------------------
 * All "list-style" overlays (vsearch, outline, backlinks, tags, picker) share
 * the same layout: centered box, header rows on top, scrollable rows in the
 * middle, hint row at the bottom. These two helpers + per-overlay geom
 * wrappers below handle drawing + grabbing the thumb uniformly. */

static int overlay_list_scrollbar_geom(int box_x, int box_w,
                                       int rows_top, int rows_bot,
                                       int content_h, int scroll,
                                       SDL_Rect* track_out, SDL_Rect* thumb_out)
{
    int visible_h = rows_bot - rows_top;
    if (content_h <= visible_h || visible_h <= 20) return 0;
    int track_w = 10;
    int track_x = box_x + box_w - track_w - 2;
    SDL_Rect track = { track_x, rows_top, track_w, visible_h };
    SDL_Rect inner; sb_inner_track(&track, &inner);
    int thumb_h = inner.h * visible_h / content_h;
    if (thumb_h < 24) thumb_h = 24;
    if (thumb_h > inner.h - 4) thumb_h = inner.h - 4;
    if (thumb_h < 12) thumb_h = inner.h;     /* degenerate; renders flat */
    int max_scroll_v = content_h - visible_h;
    int max_y = inner.h - thumb_h;
    int thumb_y = inner.y +
                  (max_y > 0 && max_scroll_v > 0
                   ? max_y * scroll / max_scroll_v : 0);
    if (track_out) *track_out = track;
    if (thumb_out) *thumb_out = (SDL_Rect){ track_x + 2, thumb_y,
                                            track_w - 4, thumb_h };
    return 1;
}

static void overlay_scrollbar_draw(App* a, const SDL_Rect* track,
                                   const SDL_Rect* thumb, bool dragging)
{
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 30);
    SDL_RenderFillRect(a->renderer, track);
    int alpha = dragging ? 220 : 150;
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, alpha);
    SDL_RenderFillRect(a->renderer, thumb);
    sb_draw_arrows(a, track, alpha);
}

/* Translate a mouse-y during drag into a clamped scroll value. Used by every
 * overlay drag-motion case. inner_y/inner_h describe the thumb travel area
 * (the track minus arrow buttons). max_sc is the maximum scroll value. */
static int scroll_from_thumb_drag(int mouse_y, int inner_y, int inner_h,
                                  int thumb_h, int drag_offset, int max_sc)
{
    int new_thumb_y = mouse_y - inner_y - drag_offset;
    int max_y = inner_h - thumb_h;
    if (new_thumb_y < 0)     new_thumb_y = 0;
    if (new_thumb_y > max_y) new_thumb_y = max_y;
    if (max_sc <= 0 || max_y <= 0) return 0;
    int sc = max_sc * new_thumb_y / max_y;
    if (sc < 0) sc = 0;
    return sc;
}

/* If the click landed on the scrollbar, dispatch it: arrow buttons step by
 * step_px, thumb begins a drag, inner-track jump centers the thumb on cursor.
 * step_px is one row / line_step for the corresponding overlay. Returns true
 * if the click was consumed. */
static bool overlay_scrollbar_handle_click(App* a,
                                           int btn_x, int btn_y,
                                           const SDL_Rect* track,
                                           const SDL_Rect* thumb,
                                           int sb_kind,
                                           int* scroll, int content_h,
                                           int step_px)
{
    if (btn_x < track->x || btn_x >= track->x + track->w ||
        btn_y < track->y || btn_y >= track->y + track->h) return false;

    int max_sc = content_h - track->h;
    if (max_sc < 0) max_sc = 0;

    /* Arrow buttons take priority over thumb/track-jump. */
    if (sb_track_has_arrows(track)) {
        SDL_Rect up, dn;
        sb_arrow_rects(track, &up, &dn);
        if (btn_y >= up.y && btn_y < up.y + up.h) {
            *scroll -= step_px;
            if (*scroll < 0) *scroll = 0;
            return true;
        }
        if (btn_y >= dn.y && btn_y < dn.y + dn.h) {
            *scroll += step_px;
            if (*scroll > max_sc) *scroll = max_sc;
            return true;
        }
    }

    SDL_Rect inner; sb_inner_track(track, &inner);
    if (btn_y >= thumb->y && btn_y < thumb->y + thumb->h) {
        a->sb_drag        = sb_kind;
        a->sb_drag_offset = btn_y - thumb->y;
    } else {
        *scroll = scroll_from_thumb_drag(btn_y, inner.y, inner.h,
                                         thumb->h, thumb->h / 2, max_sc);
        a->sb_drag        = sb_kind;
        a->sb_drag_offset = thumb->h / 2;
    }
    return true;
}

/* ----------------------------- backlink updates ------------------------- */

/* Portable case-insensitive byte compare for ASCII bytes only — good enough
 * for filenames and Markdown identifiers we control. */
static int icmp_n(const char* a, const char* b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return x - y;
    }
    return 0;
}

/* Forward decl. */
static char* slurp(const char* path, size_t* out_len);

/* Rewrite all `[[old]]` occurrences (case-insensitive name match) in `path`
 * to `[[new]]`. Returns the number of replacements made, or 0 on no-op
 * (file unchanged). Returns -1 on read/write failure. */
static int rewrite_backlinks_in_file(const char* path,
                                     const char* old_name,
                                     const char* new_name)
{
    size_t flen = 0;
    char*  data = slurp(path, &flen);
    if (!data) return -1;

    size_t old_len = strlen(old_name);
    size_t new_len = strlen(new_name);
    /* Worst-case grown size; resize as we go. */
    size_t out_cap = flen + 128;
    char*  out     = malloc(out_cap);
    size_t out_len = 0;
    int    count   = 0;

    for (size_t i = 0; i < flen; ) {
        if (i + 4 + old_len <= flen &&
            data[i] == '[' && data[i + 1] == '[' &&
            data[i + 2 + old_len] == ']' &&
            data[i + 3 + old_len] == ']' &&
            icmp_n(data + i + 2, old_name, old_len) == 0)
        {
            /* Need room for "[[new_name]]". */
            while (out_len + new_len + 4 > out_cap) {
                out_cap *= 2;
                out = realloc(out, out_cap);
            }
            memcpy(out + out_len, "[[", 2);            out_len += 2;
            memcpy(out + out_len, new_name, new_len);  out_len += new_len;
            memcpy(out + out_len, "]]", 2);            out_len += 2;
            i += 2 + old_len + 2;
            count++;
            continue;
        }
        if (out_len + 1 > out_cap) {
            out_cap *= 2;
            out = realloc(out, out_cap);
        }
        out[out_len++] = data[i++];
    }

    int rc = count;
    if (count > 0) {
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(out, 1, out_len, f);
            fclose(f);
        } else {
            rc = -1;
        }
    }
    free(data);
    free(out);
    return rc;
}

/* Strip a path down to the basename minus `.md`. Writes a NUL-terminated
 * result to `out` (capped at `cap`). Returns the resulting length. */
static size_t basename_no_md(const char* path, char* out, size_t cap)
{
    if (cap == 0) return 0;
    const char* base = vault_basename(path);
    size_t n = strlen(base);
    if (n > 3 && (base[n-3] == '.') &&
        (base[n-2] == 'm' || base[n-2] == 'M') &&
        (base[n-1] == 'd' || base[n-1] == 'D'))
        n -= 3;
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = 0;
    return n;
}

/* Walk every .md in the vault, rewriting `[[old_basename]]` → `[[new_basename]]`.
 * Returns the number of FILES touched. Reloads the open buffer if it was
 * one of the touched files. */
static int update_backlinks_in_vault(App* a,
                                     const char* old_basename,
                                     const char* new_basename)
{
    if (!a->vault.dir || !old_basename || !*old_basename ||
        !new_basename || strcmp(old_basename, new_basename) == 0) return 0;
    int files_touched = 0;
    for (size_t i = 0; i < a->vault.count; ++i) {
        if (a->vault.items[i].is_dir)   continue;
        if (a->vault.items[i].is_image) continue;
        int n = rewrite_backlinks_in_file(a->vault.items[i].path,
                                          old_basename, new_basename);
        if (n > 0) {
            files_touched++;
            /* If this happens to be the currently open buffer, reload it
             * from disk so the on-screen text matches what we just wrote. */
            if (a->note_path &&
                strcmp(a->note_path, a->vault.items[i].path) == 0)
            {
                load_note(a, a->vault.items[i].path);
            }
        }
    }
    return files_touched;
}

/* Show a transient status-bar notification. */
static void app_notify(App* a, const char* msg)
{
    free(a->notification_msg);
    a->notification_msg   = strdup(msg);
    a->notification_until = SDL_GetTicks() + 3500;
}

/* ----------------------------- context menus --------------------------- */

/* The context menu is shared between two kinds: the sidebar item menu
 * (Open/Rename/Delete/New) and the editor formatting menu opened by a
 * right-click in edit mode (Bold/Italic/Heading/...). The kind is stored
 * in App::ctx_menu_kind and dispatched via the helpers below. */
#define CTX_KIND_SIDEBAR 0
#define CTX_KIND_EDITOR  1
#define CTX_KIND_MENU    2     /* title-bar File/Edit/View/Help dropdowns */

/* Title-bar menu definitions: a (label, shortcut, action) per menu item.
 * ctx_menu_target stores which menu is open (0=File, 1=Edit, 2=View,
 * 3=Help). Per-row dispatch in ctx_menu_invoke_row. */
typedef struct {
    const char* label;
    const char* shortcut;
    void (*fn)(App*);
} MenuItem;

/* Forward decls for all action_* funcs referenced in the tables below. */
static void action_save          (App* a);
static void action_save_as       (App* a);
static void action_new_file      (App* a);
static void action_open_file     (App* a);
static void action_open_dir      (App* a);
static void action_quit          (App* a);
static void action_undo          (App* a);
static void action_redo          (App* a);
static void action_cut           (App* a);
static void action_copy          (App* a);
static void action_paste         (App* a);
static void action_select_all    (App* a);
static void action_find_replace  (App* a);
static void action_quick_switch  (App* a);
static void action_command_palette(App* a);
static void action_plugins        (App* a);
static void action_outline       (App* a);
static void action_backlinks     (App* a);
static void action_tags          (App* a);
static void action_help          (App* a);
static void action_keybindings   (App* a);
static void action_colors        (App* a);

static const MenuItem MENU_FILE[] = {
    { "New",                "Ctrl+N",       action_new_file },
    { "Open\xe2\x80\xa6",   "Ctrl+O",       action_open_file },
    { "Open Dir\xe2\x80\xa6",NULL,          action_open_dir },
    { "Quick switch",       "Ctrl+P",       action_quick_switch },
    { "Save",               "Ctrl+S",       action_save },
    { "Save As\xe2\x80\xa6","Ctrl+Shift+S", action_save_as },
    { "Quit",               "Ctrl+Q",       action_quit },
    { NULL, NULL, NULL }
};
static const MenuItem MENU_EDIT[] = {
    { "Undo",         "Ctrl+Z", action_undo },
    { "Redo",         "Ctrl+Y", action_redo },
    { "Cut",          "Ctrl+X", action_cut },
    { "Copy",         "Ctrl+C", action_copy },
    { "Paste",        "Ctrl+V", action_paste },
    { "Select All",   "Ctrl+A", action_select_all },
    { "Find",         "Ctrl+F", action_find },
    { "Find / Replace","Ctrl+H",action_find_replace },
    { "Vault Search", NULL,     action_vsearch },
    { NULL, NULL, NULL }
};
static const MenuItem MENU_VIEW[] = {
    { "Toggle Edit/Preview", "Ctrl+E", action_toggle_edit },
    { "Toggle Sidebar",      "Ctrl+B", action_toggle_sidebar },
    { "Outline Panel",       NULL,     action_outline_pin },
    { "Outline\xe2\x80\xa6",  NULL,     action_outline },
    { "Backlinks\xe2\x80\xa6",NULL,    action_backlinks },
    { "Tags\xe2\x80\xa6",     NULL,    action_tags },
    { "Settings\xe2\x80\xa6", NULL,    action_settings },
    { NULL, NULL, NULL }
};
static void action_about         (App* a);
static const MenuItem MENU_HELP[] = {
    { "Help",         "F1",     action_help },
    { "Keybindings\xe2\x80\xa6","Ctrl+K",action_keybindings },
    { "Color Picker\xe2\x80\xa6",NULL,action_colors },
    { "About Downsee\xe2\x80\xa6",NULL, action_about },
    { NULL, NULL, NULL }
};
static const MenuItem* MENU_TABLES[4] = {
    MENU_FILE, MENU_EDIT, MENU_VIEW, MENU_HELP
};
/* Static accessors used by the menu rendering — the static menu tables
 * don't carry an App pointer, so dynamic content (recent vaults) is
 * fetched off the singleton wndproc App pointer. */
static int app_recent_dirs_count(void)
{
    return g_app_for_wndproc ? g_app_for_wndproc->recent_dirs_count : 0;
}
static const char* app_recent_dir_at(int i)
{
    if (!g_app_for_wndproc) return "";
    if (i < 0 || i >= g_app_for_wndproc->recent_dirs_count) return "";
    return g_app_for_wndproc->recent_dirs[i];
}

static int menu_count_static(int idx) {
    if (idx < 0 || idx >= 4) return 0;
    const MenuItem* m = MENU_TABLES[idx];
    int n = 0; while (m[n].label) n++; return n;
}

/* Forward decl so menu_count can know how many recent dirs to surface
 * under the File menu without referring to the App struct directly. */
static int  app_recent_dirs_count(void);
static const char* app_recent_dir_at(int i);

static int menu_count(int idx) {
    int n = menu_count_static(idx);
    if (idx == 0 && app_recent_dirs_count() > 0) {
        /* +1 for the "Recent vaults" submenu row. The recent entries
         * themselves live in a child popup, not in this list. */
        n += 1;
    }
    return n;
}

/* True if the row at `row` of the currently-open menu is the
 * "Recent vaults" entry that opens the inner submenu. */
static bool ctx_is_recent_submenu_row(const App* a, int row)
{
    if (a->ctx_menu_kind != CTX_KIND_MENU) return false;
    if (a->ctx_menu_target != 0)           return false;
    if (app_recent_dirs_count() <= 0)      return false;
    int sn = menu_count_static(0);
    return row == sn;
}

/* Sidebar menu actions. */
typedef enum {
    CTX_OPEN,
    CTX_RENAME,
    CTX_DELETE,
    CTX_NEW_FILE,
    CTX_NEW_DIR,
    CTX_COUNT,
} CtxAction;

static const char* CTX_LABELS[CTX_COUNT] = {
    "Open",
    "Rename…",
    "Delete",
    "New file here…",
    "New folder here…",
};

/* Editor formatting menu actions, ordered as they appear in the popup. */
typedef enum {
    ED_BOLD,
    ED_ITALIC,
    ED_CODE,
    ED_STRIKE,
    ED_LINK,
    ED_H1,
    ED_H2,
    ED_H3,
    ED_QUOTE,
    ED_LIST,
    ED_COUNT,
} EditorAction;

static const char* ED_LABELS[ED_COUNT] = {
    "Bold",
    "Italic",
    "Inline code",
    "Strikethrough",
    "Link",
    "Heading 1",
    "Heading 2",
    "Heading 3",
    "Quote",
    "Bullet list",
};

/* Right-side hint shown in the popup, mirroring common editor shortcuts. */
static const char* ED_SHORTCUTS[ED_COUNT] = {
    "Ctrl+B",
    "Ctrl+I",
    "Ctrl+`",
    "",
    "Ctrl+K",
    "#",
    "##",
    "###",
    ">",
    "-",
};

/* Returns 1 if sidebar action `a` applies to the current target. */
static int ctx_action_applies(const App* app, CtxAction a)
{
    int idx = app->ctx_menu_target;
    if (idx < 0) {                                 /* empty-area menu */
        return a == CTX_NEW_FILE || a == CTX_NEW_DIR;
    }
    const VaultItem* it = &app->vault.items[idx];
    if (it->is_dir) {
        return a == CTX_NEW_FILE || a == CTX_NEW_DIR
            || a == CTX_RENAME || a == CTX_DELETE;
    }
    /* file */
    return a == CTX_OPEN || a == CTX_RENAME || a == CTX_DELETE
        || a == CTX_NEW_FILE || a == CTX_NEW_DIR;
}

static int ctx_sidebar_visible_count(const App* app)
{
    int n = 0;
    for (int i = 0; i < CTX_COUNT; ++i)
        if (ctx_action_applies(app, (CtxAction)i)) n++;
    return n;
}

/* Map a visible-row index back to the underlying CtxAction. */
static CtxAction ctx_action_at_row(const App* app, int row)
{
    int n = 0;
    for (int i = 0; i < CTX_COUNT; ++i) {
        if (!ctx_action_applies(app, (CtxAction)i)) continue;
        if (n == row) return (CtxAction)i;
        n++;
    }
    return CTX_COUNT;
}

/* Total row count for whichever kind is active. */
static int ctx_visible_count(const App* a)
{
    if (a->ctx_menu_kind == CTX_KIND_EDITOR) return ED_COUNT;
    if (a->ctx_menu_kind == CTX_KIND_MENU)   return menu_count(a->ctx_menu_target);
    return ctx_sidebar_visible_count(a);
}

static const char* ctx_label_at(const App* a, int row)
{
    if (a->ctx_menu_kind == CTX_KIND_EDITOR) {
        if (row < 0 || row >= ED_COUNT) return "";
        return ED_LABELS[row];
    }
    if (a->ctx_menu_kind == CTX_KIND_MENU) {
        int n = menu_count(a->ctx_menu_target);
        if (row < 0 || row >= n) return "";
        int sn = menu_count_static(a->ctx_menu_target);
        if (row < sn) return MENU_TABLES[a->ctx_menu_target][row].label;
        if (a->ctx_menu_target == 0 && row == sn) return "Recent vaults";
        return "";
    }
    CtxAction act = ctx_action_at_row(a, row);
    return (act < CTX_COUNT) ? CTX_LABELS[act] : "";
}

static const char* ctx_shortcut_at(const App* a, int row)
{
    if (a->ctx_menu_kind == CTX_KIND_EDITOR) {
        if (row < 0 || row >= ED_COUNT) return "";
        return ED_SHORTCUTS[row];
    }
    if (a->ctx_menu_kind == CTX_KIND_MENU) {
        int n = menu_count(a->ctx_menu_target);
        if (row < 0 || row >= n) return "";
        int sn = menu_count_static(a->ctx_menu_target);
        if (row < sn) {
            const char* s = MENU_TABLES[a->ctx_menu_target][row].shortcut;
            return s ? s : "";
        }
        return "";
    }
    return "";
}

static int ctx_menu_row_h(const App* a) { return font_line_height(a->font_body) + 8; }
static int ctx_menu_w   (const App* a)
{
    if (a->ctx_menu_kind == CTX_KIND_EDITOR) return 240;
    if (a->ctx_menu_kind == CTX_KIND_MENU) {
        /* Auto-size: widest label + widest shortcut + a fixed gap so the
         * shortcut text never overlaps the label. */
        int n = ctx_visible_count(a);
        int max_label = 0, max_short = 0;
        for (int i = 0; i < n; ++i) {
            const char* L = ctx_label_at(a, i);
            const char* S = ctx_shortcut_at(a, i);
            int lw = font_measure(a->font_body, L, strlen(L));
            int sw = (S && *S) ? font_measure(a->font_body, S, strlen(S)) : 0;
            if (lw > max_label) max_label = lw;
            if (sw > max_short) max_short = sw;
        }
        /* Padding: 14 left + 24 gap + 12 right. */
        int w = max_label + 24 + max_short + 26;
        if (w < 220) w = 220;
        if (w > 360) w = 360;
        return w;
    }
    return 200;
}

static void ctx_menu_open(App* a, int x, int y, int target_idx)
{
    a->ctx_menu_active = true;
    a->ctx_menu_kind   = CTX_KIND_SIDEBAR;
    a->ctx_menu_x      = x;
    a->ctx_menu_y      = y;
    a->ctx_menu_target = target_idx;
    a->ctx_menu_hover  = 0;     /* select first row by default for keyboard */
    /* Reset open-anim and per-row hover state so we get a fresh fade-in. */
    a->ctx_menu_open_t = 0.0f;
    for (int r = 0; r < 16; ++r) a->ctx_menu_row_t[r] = 0.0f;
    /* Keep popup on-screen. */
    int rh = ctx_menu_row_h(a);
    int h  = rh * ctx_visible_count(a) + 8;
    if (a->ctx_menu_x + ctx_menu_w(a) > a->win_w - 4)
        a->ctx_menu_x = a->win_w - 4 - ctx_menu_w(a);
    if (a->ctx_menu_y + h > a->win_h - 4)
        a->ctx_menu_y = a->win_h - 4 - h;
    if (a->ctx_menu_x < 4) a->ctx_menu_x = 4;
    if (a->ctx_menu_y < 4) a->ctx_menu_y = 4;
}

static void ctx_menu_open_editor(App* a, int x, int y)
{
    a->ctx_menu_active = true;
    a->ctx_menu_kind   = CTX_KIND_EDITOR;
    a->ctx_menu_x      = x;
    a->ctx_menu_y      = y;
    a->ctx_menu_target = -1;
    a->ctx_menu_hover  = 0;
    a->ctx_menu_open_t = 0.0f;
    for (int r = 0; r < 16; ++r) a->ctx_menu_row_t[r] = 0.0f;
    int rh = ctx_menu_row_h(a);
    int h  = rh * ctx_visible_count(a) + 8;
    if (a->ctx_menu_x + ctx_menu_w(a) > a->win_w - 4)
        a->ctx_menu_x = a->win_w - 4 - ctx_menu_w(a);
    if (a->ctx_menu_y + h > a->win_h - 4)
        a->ctx_menu_y = a->win_h - 4 - h;
    if (a->ctx_menu_x < 4) a->ctx_menu_x = 4;
    if (a->ctx_menu_y < 4) a->ctx_menu_y = 4;
}

/* Open a title-bar dropdown menu. menu_idx is 0..3 (File/Edit/View/Help). */
static void ctx_menu_open_menu(App* a, int menu_idx, int x, int y)
{
    a->ctx_menu_active = true;
    a->ctx_menu_kind   = CTX_KIND_MENU;
    a->ctx_menu_target = menu_idx;
    a->ctx_menu_x      = x;
    a->ctx_menu_y      = y;
    a->ctx_menu_hover  = 0;
    a->ctx_menu_open_t = 0.0f;
    for (int r = 0; r < 16; ++r) a->ctx_menu_row_t[r] = 0.0f;
    int rh = ctx_menu_row_h(a);
    int h  = rh * ctx_visible_count(a) + 8;
    if (a->ctx_menu_x + ctx_menu_w(a) > a->win_w - 4)
        a->ctx_menu_x = a->win_w - 4 - ctx_menu_w(a);
    if (a->ctx_menu_y + h > a->win_h - 4)
        a->ctx_menu_y = a->win_h - 4 - h;
    if (a->ctx_menu_x < 4) a->ctx_menu_x = 4;
    if (a->ctx_menu_y < 4) a->ctx_menu_y = 4;
}

static void ctx_menu_close(App* a) {
    a->ctx_menu_active = false;
    a->ctx_submenu_active = false;
    a->ctx_submenu_hover = -1;
    a->menu_open = -1;
}

/* ----------------------------- recent-vaults submenu ------------------- */

static int submenu_row_h(const App* a)
{
    return font_line_height(a->font_body) + 8;
}

static int submenu_w(const App* a)
{
    int n = app_recent_dirs_count();
    if (n <= 0) return 220;
    int max_label = 0;
    for (int i = 0; i < n; ++i) {
        const char* L = app_recent_dir_at(i);
        int lw = font_measure(a->font_body, L, strlen(L));
        if (lw > max_label) max_label = lw;
    }
    int w = max_label + 26;          /* 14 left pad + 12 right pad */
    if (w < 220) w = 220;
    if (w > 480) w = 480;            /* clamp so very long paths don't overflow */
    return w;
}

static int submenu_h(const App* a)
{
    int n = app_recent_dirs_count();
    if (n > 16) n = 16;               /* hard cap to keep row_t array sized */
    return submenu_row_h(a) * n + 8;
}

static SDL_Rect submenu_box_rect(const App* a)
{
    int sn = menu_count_static(0);
    int parent_rh = ctx_menu_row_h(a);
    int parent_w  = ctx_menu_w(a);
    /* Anchored to the right edge of the parent menu, vertically aligned
     * with the recent-vaults row. */
    int x = a->ctx_menu_x + parent_w - 2;
    int y = a->ctx_menu_y + 4 + sn * parent_rh - 4;
    int w = submenu_w(a);
    int h = submenu_h(a);
    /* Flip to the left if the submenu would overflow the right edge. */
    if (x + w > a->win_w - 4) x = a->ctx_menu_x - w + 2;
    if (x < 4) x = 4;
    if (y + h > a->win_h - 4) y = a->win_h - 4 - h;
    if (y < 4) y = 4;
    return (SDL_Rect){ x, y, w, h };
}

static int submenu_row_at(const App* a, int mx, int my)
{
    if (!a->ctx_submenu_active) return -1;
    SDL_Rect b = submenu_box_rect(a);
    if (mx < b.x || mx >= b.x + b.w) return -1;
    if (my < b.y || my >= b.y + b.h) return -1;
    int rh = submenu_row_h(a);
    int row = (my - b.y - 4) / rh;
    int n = app_recent_dirs_count();
    if (n > 16) n = 16;
    if (row < 0 || row >= n) return -1;
    return row;
}

/* Open the recent vault at submenu row `row` and close everything. */
static void submenu_invoke_row(App* a, int row)
{
    int n = app_recent_dirs_count();
    if (row < 0 || row >= n) { ctx_menu_close(a); return; }
    const char* path = app_recent_dir_at(row);
    ctx_menu_close(a);
    if (path && *path) {
        char snap[1024];
        snprintf(snap, sizeof snap, "%s", path);
        int nfound = vault_scan(&a->vault, snap);
        recent_dirs_push(a, snap);
        settings_persist(a);
        char msg[300];
        snprintf(msg, sizeof msg, "vault: %s (%d note%s)",
                 snap, nfound, nfound == 1 ? "" : "s");
        app_notify(a, msg);
    }
}

/* Translate a screen y to the row index inside the menu, or -1 if outside. */
static int ctx_menu_row_at(const App* a, int mx, int my)
{
    if (!a->ctx_menu_active) return -1;
    int rh = ctx_menu_row_h(a);
    int h  = rh * ctx_visible_count(a) + 8;
    if (mx < a->ctx_menu_x || mx >= a->ctx_menu_x + ctx_menu_w(a)) return -1;
    if (my < a->ctx_menu_y || my >= a->ctx_menu_y + h)            return -1;
    int row = (my - a->ctx_menu_y - 4) / rh;
    if (row < 0 || row >= ctx_visible_count(a)) return -1;
    return row;
}

static void render_context_menu(App* a)
{
    /* Render while the open animation is still playing (closed but t > 0)
     * so we get a fade-out, not a snap. */
    if (!a->ctx_menu_active && a->ctx_menu_open_t < 0.01f) return;
    int rh = ctx_menu_row_h(a);
    int rows = ctx_visible_count(a);
    int w  = ctx_menu_w(a);
    int h  = rh * rows + 8;

    /* Fade-in: slight rise from below + alpha fade. */
    float et = ease_out_cubic(a->ctx_menu_open_t);
    int rise = (int)((1.0f - et) * 6.0f);
    int alpha = (int)(255.0f * et);

    SDL_Rect box = { a->ctx_menu_x, a->ctx_menu_y + rise, w, h };
    /* Drop shadow: darker, larger rect behind the card for elevation. */
    SDL_Rect shadow = { box.x - 1, box.y + 4, box.w + 2, box.h + 4 };
    SDL_SetRenderDrawColor(a->renderer, 0, 0, 0, (Uint8)(80 * et));
    fill_rrect(a->renderer, shadow, 8);
    overlay_card(a, box);

    int y = box.y + 4;
    for (int r = 0; r < rows && r < 16; ++r) {
        float rt = ease_out_cubic(a->ctx_menu_row_t[r]);
        /* Hover row: animated bg fill that slides in from the left as a
         * subtle 3px accent strip + bg tint. */
        if (rt > 0.01f) {
            SDL_Rect hr = { box.x + 2, y, w - 4, rh };
            SDL_SetRenderDrawColor(a->renderer,
                a->bg_sidebar_hover.r, a->bg_sidebar_hover.g,
                a->bg_sidebar_hover.b, (Uint8)(220 * rt * et));
            fill_rrect(a->renderer, hr, 4);
            /* Accent stripe on the left edge — grows top-to-bottom feel. */
            SDL_Rect bar = { box.x + 2, y + 2, 2, rh - 4 };
            SDL_SetRenderDrawColor(a->renderer,
                a->fg_link.r, a->fg_link.g, a->fg_link.b,
                (Uint8)(220 * rt * et));
            fill_rrect(a->renderer, bar, 1);
        }
        const char* label = ctx_label_at(a, r);
        const char* shortc = ctx_shortcut_at(a, r);
        /* Cross-fade label color from fg to fg_link as hover ramps. */
        int lr = (int)(a->fg.r + (a->fg_link.r - a->fg.r) * rt);
        int lg = (int)(a->fg.g + (a->fg_link.g - a->fg.g) * rt);
        int lb = (int)(a->fg.b + (a->fg_link.b - a->fg.b) * rt);
        SDL_Color c  = { (Uint8)lr, (Uint8)lg, (Uint8)lb, (Uint8)alpha };
        font_draw_line(a->font_body, label, strlen(label),
                       box.x + 14,
                       y + font_ascent(a->font_body) + 3, c);
        if (shortc && *shortc) {
            int sw = font_measure(a->font_body, shortc, strlen(shortc));
            SDL_Color sc = a->fg_muted; sc.a = (Uint8)(alpha * 0.7f);
            font_draw_line(a->font_body, shortc, strlen(shortc),
                           box.x + w - sw - 12,
                           y + font_ascent(a->font_body) + 3, sc);
        } else if (ctx_is_recent_submenu_row(a, r)) {
            /* Submenu chevron on the right edge — same color cross-fade
             * as the label so it tints with hover. */
            const char* chev = ">";
            int cw_ = font_measure(a->font_body, chev, strlen(chev));
            font_draw_line(a->font_body, chev, strlen(chev),
                           box.x + w - cw_ - 12,
                           y + font_ascent(a->font_body) + 3, c);
        }
        y += rh;
    }
}

/* Recent-vaults submenu card. Renders only while open (or fading). Same
 * card/shadow/row treatment as the parent context menu so it visually
 * reads as a child of it. */
static void render_recent_submenu(App* a)
{
    if (!a->ctx_submenu_active && a->ctx_submenu_open_t < 0.01f) return;
    if (a->ctx_menu_kind != CTX_KIND_MENU || a->ctx_menu_target != 0) return;
    int n = app_recent_dirs_count();
    if (n <= 0) return;
    if (n > 16) n = 16;

    SDL_Rect box = submenu_box_rect(a);
    int rh = submenu_row_h(a);
    float et = ease_out_cubic(a->ctx_submenu_open_t);
    int rise = (int)((1.0f - et) * 6.0f);
    int alpha = (int)(255.0f * et);
    box.y += rise;

    SDL_Rect shadow = { box.x - 1, box.y + 4, box.w + 2, box.h + 4 };
    SDL_SetRenderDrawColor(a->renderer, 0, 0, 0, (Uint8)(80 * et));
    fill_rrect(a->renderer, shadow, 8);
    overlay_card(a, box);

    int y = box.y + 4;
    for (int r = 0; r < n; ++r) {
        float rt = ease_out_cubic(a->ctx_submenu_row_t[r]);
        if (rt > 0.01f) {
            SDL_Rect hr = { box.x + 2, y, box.w - 4, rh };
            SDL_SetRenderDrawColor(a->renderer,
                a->bg_sidebar_hover.r, a->bg_sidebar_hover.g,
                a->bg_sidebar_hover.b, (Uint8)(220 * rt * et));
            fill_rrect(a->renderer, hr, 4);
            SDL_Rect bar = { box.x + 2, y + 2, 2, rh - 4 };
            SDL_SetRenderDrawColor(a->renderer,
                a->fg_link.r, a->fg_link.g, a->fg_link.b,
                (Uint8)(220 * rt * et));
            fill_rrect(a->renderer, bar, 1);
        }
        const char* label = app_recent_dir_at(r);
        int lr = (int)(a->fg.r + (a->fg_link.r - a->fg.r) * rt);
        int lg = (int)(a->fg.g + (a->fg_link.g - a->fg.g) * rt);
        int lb = (int)(a->fg.b + (a->fg_link.b - a->fg.b) * rt);
        SDL_Color c = { (Uint8)lr, (Uint8)lg, (Uint8)lb, (Uint8)alpha };
        /* Clip the path text to the row width so over-long paths don't
         * spill out of the card. */
        SDL_Rect clip = { box.x + 4, y, box.w - 8, rh };
        SDL_RenderSetClipRect(a->renderer, &clip);
        font_draw_line(a->font_body, label, strlen(label),
                       box.x + 14,
                       y + font_ascent(a->font_body) + 3, c);
        SDL_RenderSetClipRect(a->renderer, NULL);
        y += rh;
    }
}

/* Confirm dialog: returns 1 = yes, 0 = no/cancel. Win32 uses a native
 * MessageBox; other platforms default to 1 (no easy modal). */
static int confirm_yesno(SDL_Window* parent, const char* title, const char* msg)
{
#if defined(_WIN32)
    SDL_SysWMinfo info; SDL_VERSION(&info.version);
    HWND hwnd = NULL;
    if (parent && SDL_GetWindowWMInfo(parent, &info)) hwnd = info.info.win.window;
    int r = MessageBoxA(hwnd, msg, title, MB_YESNO | MB_ICONQUESTION);
    return r == IDYES;
#else
    (void)parent; (void)title; (void)msg;
    return 1;
#endif
}

/* True if href is something we can hand off to the OS shell (web URL,
 * mailto, ftp, file). The wiki-link path handles internal [[name]] links;
 * everything else routes through the external opener. */
static bool is_external_url(const char* href)
{
    if (!href || !*href) return false;
    static const char* schemes[] = {
        "http://", "https://", "mailto:", "ftp://", "ftps://",
        "file://", "irc://", "ircs://", "ssh://", "tel:",
    };
    for (size_t i = 0; i < sizeof schemes / sizeof schemes[0]; ++i) {
        size_t n = strlen(schemes[i]);
        if (strncasecmp(href, schemes[i], n) == 0) return true;
    }
    return false;
}

/* Hand the URL to the OS. ShellExecute on Windows lets the user's default
 * browser/mail client open it; xdg-open does the same on Linux. */
static void open_external_url(const char* href)
{
#if defined(_WIN32)
    ShellExecuteA(NULL, "open", href, NULL, NULL, SW_SHOWNORMAL);
#else
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "xdg-open '%s' >/dev/null 2>&1 &", href);
    int rc = system(cmd);
    (void)rc;
#endif
}

/* Delete a file (or empty dir). Returns 0 on success. */
static int filesystem_delete(const char* path, int is_dir)
{
#if defined(_WIN32)
    if (is_dir) return RemoveDirectoryA(path) ? 0 : -1;
    return DeleteFileA(path) ? 0 : -1;
#else
    return is_dir ? rmdir(path) : unlink(path);
#endif
}

/* Build the directory portion of `path` into `out`. Returns 0 on success.
 * For a top-level file `foo.md`, `out` is empty. */
static int dirname_of(const char* path, char* out, size_t cap)
{
    size_t n = strlen(path);
    while (n > 0 && path[n - 1] != '/' && path[n - 1] != '\\') n--;
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\')) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = 0;
    return 0;
}

/* Dispatch the chosen menu action. */
static void ctx_menu_invoke(App* a, CtxAction act)
{
    int idx = a->ctx_menu_target;
    const VaultItem* it = (idx >= 0) ? &a->vault.items[idx] : NULL;
    ctx_menu_close(a);

    switch (act) {
        case CTX_OPEN: {
            if (!it || it->is_dir) return;
            if (!confirm_discard(a)) return;
            load_note(a, it->path);
            break;
        }
        case CTX_RENAME: {
            if (!it) return;
            /* In-app text modal: prefill with current basename. New name
             * resolves relative to the item's parent dir. */
            {
                char rdir[1024];
                dirname_of(it->path, rdir, sizeof rdir);
                if (!app_text_modal(a, "Rename to", it->name, rdir)) return;
            }
            if (a->tinput_len == 0) return;
            char picked[1024];
            bool is_abs = (a->tinput_text[0] == '/' || a->tinput_text[0] == '\\' ||
                           (a->tinput_len >= 2 && a->tinput_text[1] == ':'));
            if (is_abs) snprintf(picked, sizeof picked, "%s", a->tinput_text);
            else if (a->tinput_dir[0]) path_join_safe(picked, sizeof picked,
                                                      a->tinput_dir, a->tinput_text);
            else snprintf(picked, sizeof picked, "%s", a->tinput_text);
            if (strcmp(picked, it->path) != 0) {
                char old_base[256], new_base[256];
                basename_no_md(it->path, old_base, sizeof old_base);
                basename_no_md(picked,   new_base, sizeof new_base);
                if (rename(it->path, picked) == 0) {
                    if (a->note_path && strcmp(a->note_path, it->path) == 0) {
                        free(a->note_path);
                        a->note_path = strdup(picked);
                        update_window_title(a);
                    }
                    if (a->vault.dir) vault_scan(&a->vault, a->vault.dir);
                    if (a->note_path)
                        a->vault.selected = vault_index_of(&a->vault, a->note_path);
                    if (!it->is_dir) {
                        int touched = update_backlinks_in_vault(
                            a, old_base, new_base);
                        char msg[160];
                        if (touched > 0) {
                            snprintf(msg, sizeof msg,
                                "renamed: updated [[%.40s]] -> [[%.40s]] in %d file(s)",
                                old_base, new_base, touched);
                        } else {
                            snprintf(msg, sizeof msg,
                                "renamed (no backlinks to update)");
                        }
                        app_notify(a, msg);
                    }
                } else {
                    fprintf(stderr, "rename failed: %s -> %s\n",
                            it->path, picked);
                }
            }
            break;
        }
        case CTX_DELETE: {
            if (!it) return;
            char prompt[1024];
            snprintf(prompt, sizeof prompt,
                "Delete \"%s\"%s?\nThis cannot be undone.",
                it->name, it->is_dir ? " (folder must be empty)" : "");
            if (!confirm_yesno(a->window, "Delete", prompt)) return;
            if (filesystem_delete(it->path, it->is_dir) == 0) {
                /* If we deleted the open file, drop the buffer (mark unsaved). */
                if (!it->is_dir && a->note_path &&
                    strcmp(a->note_path, it->path) == 0)
                {
                    free(a->note_path);
                    a->note_path = strdup("(unsaved)");
                    a->buf.dirty = true;
                    update_window_title(a);
                }
                if (a->vault.dir) vault_scan(&a->vault, a->vault.dir);
                if (a->note_path)
                    a->vault.selected = vault_index_of(&a->vault, a->note_path);
            } else {
                fprintf(stderr, "delete failed: %s\n", it->path);
            }
            break;
        }
        case CTX_NEW_FILE: {
            /* Default location: directory of the right-clicked target, or
             * the vault root when right-clicking empty area. In-app text
             * modal asks for the new filename; resolves under the chosen dir. */
            char dir[1024] = {0};
            if (it) {
                if (it->is_dir)
                    snprintf(dir, sizeof dir, "%s", it->path);
                else
                    dirname_of(it->path, dir, sizeof dir);
            } else if (a->vault.dir) {
                snprintf(dir, sizeof dir, "%s", a->vault.dir);
            }
            if (!app_text_modal(a, "New file name", "untitled.md", dir))
                return;
            if (a->tinput_len == 0) return;
            char picked[1024];
            bool is_abs = (a->tinput_text[0] == '/' || a->tinput_text[0] == '\\' ||
                           (a->tinput_len >= 2 && a->tinput_text[1] == ':'));
            if (is_abs) snprintf(picked, sizeof picked, "%s", a->tinput_text);
            else if (a->tinput_dir[0]) path_join_safe(picked, sizeof picked,
                                                      a->tinput_dir, a->tinput_text);
            else snprintf(picked, sizeof picked, "%s", a->tinput_text);
            FILE* f = fopen(picked, "wb");
            if (f) {
                fclose(f);
                if (!confirm_discard(a)) return;
                load_note(a, picked);
                if (a->vault.dir) vault_scan(&a->vault, a->vault.dir);
                a->vault.selected = vault_index_of(&a->vault, picked);
            } else {
                fprintf(stderr, "create failed: %s\n", picked);
            }
            break;
        }
        case CTX_NEW_DIR: {
            /* Same target-dir resolution as CTX_NEW_FILE — folder of
             * the right-clicked entry, or vault root for empty area. */
            char dir[1024] = {0};
            if (it) {
                if (it->is_dir)
                    snprintf(dir, sizeof dir, "%s", it->path);
                else
                    dirname_of(it->path, dir, sizeof dir);
            } else if (a->vault.dir) {
                snprintf(dir, sizeof dir, "%s", a->vault.dir);
            }
            if (!app_text_modal(a, "New folder name", "untitled", dir))
                return;
            if (a->tinput_len == 0) return;
            char picked[1024];
            bool is_abs = (a->tinput_text[0] == '/' || a->tinput_text[0] == '\\' ||
                           (a->tinput_len >= 2 && a->tinput_text[1] == ':'));
            if (is_abs) snprintf(picked, sizeof picked, "%s", a->tinput_text);
            else if (a->tinput_dir[0]) path_join_safe(picked, sizeof picked,
                                                      a->tinput_dir, a->tinput_text);
            else snprintf(picked, sizeof picked, "%s", a->tinput_text);
#ifdef _WIN32
            BOOL ok_mk = CreateDirectoryA(picked, NULL);
            int  err   = ok_mk ? 0
                : (GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1);
#else
            int err = (mkdir(picked, 0755) == 0 || errno == EEXIST) ? 0 : -1;
#endif
            if (err == 0) {
                if (a->vault.dir) vault_scan(&a->vault, a->vault.dir);
                a->vault.selected = vault_index_of(&a->vault, picked);
            } else {
                fprintf(stderr, "mkdir failed: %s\n", picked);
            }
            break;
        }
        default: break;
    }
}

/* Wrap the current selection (or a placeholder) with `prefix`/`suffix`. When
 * there's no selection, drops the markers and parks the cursor between them
 * so the user can immediately type the styled text. */
static void editor_wrap_selection(App* a, const char* prefix, const char* suffix)
{
    Buffer* b = &a->buf;
    size_t plen = strlen(prefix), slen = strlen(suffix);
    if (buffer_has_selection(b)) {
        size_t lo, hi;
        buffer_get_selection(b, &lo, &hi);
        size_t n = hi - lo;
        char* sel = (char*)malloc(n + 1);
        memcpy(sel, b->data + lo, n);
        sel[n] = 0;
        buffer_delete_selection(b);
        buffer_insert(b, prefix, plen);
        buffer_insert(b, sel, n);
        buffer_insert(b, suffix, slen);
        free(sel);
        buffer_clear_selection(b);
    } else {
        buffer_insert(b, prefix, plen);
        buffer_insert(b, suffix, slen);
        if (b->cursor >= slen) b->cursor -= slen;
    }
    b->dirty = true;
    buffer_undo_break(b);
}

/* Prepend `prefix` to the start of every line that intersects the current
 * cursor / selection. Used for headings, quotes, list items. */
static void editor_prepend_lines(App* a, const char* prefix)
{
    Buffer* b = &a->buf;
    size_t lo, hi;
    if (buffer_has_selection(b)) buffer_get_selection(b, &lo, &hi);
    else                          { lo = hi = b->cursor; }
    /* Snap lo to start of its line. */
    while (lo > 0 && b->data[lo-1] != '\n') lo--;
    /* Walk lines from lo to hi, prepending. Track inserted bytes so hi
     * shifts forward as we go. */
    size_t plen = strlen(prefix);
    size_t pos  = lo;
    while (pos <= hi && pos <= b->len) {
        b->cursor = pos;
        buffer_insert(b, prefix, plen);
        hi += plen;
        /* Skip to next line. */
        while (pos < b->len && b->data[pos] != '\n') pos++;
        if (pos < b->len) pos++;     /* step past the \n */
        else break;
    }
    buffer_clear_selection(b);
    b->dirty = true;
    buffer_undo_break(b);
}

/* Dispatch the chosen editor formatting action. */
static void ed_menu_invoke(App* a, EditorAction act)
{
    ctx_menu_close(a);
    if (!a->edit_mode) return;
    switch (act) {
        case ED_BOLD:    editor_wrap_selection(a, "**", "**");        break;
        case ED_ITALIC:  editor_wrap_selection(a, "*",  "*");         break;
        case ED_CODE:    editor_wrap_selection(a, "`",  "`");         break;
        case ED_STRIKE:  editor_wrap_selection(a, "~~", "~~");        break;
        case ED_LINK:    editor_wrap_selection(a, "[",  "](url)");    break;
        case ED_H1:      editor_prepend_lines(a, "# ");               break;
        case ED_H2:      editor_prepend_lines(a, "## ");              break;
        case ED_H3:      editor_prepend_lines(a, "### ");             break;
        case ED_QUOTE:   editor_prepend_lines(a, "> ");               break;
        case ED_LIST:    editor_prepend_lines(a, "- ");               break;
        default: break;
    }
    bump_blink(a);
}

/* Single dispatch from a row click, switches on the active menu kind. */
static void ctx_menu_invoke_row(App* a, int row)
{
    if (a->ctx_menu_kind == CTX_KIND_EDITOR) {
        if (row >= 0 && row < ED_COUNT) ed_menu_invoke(a, (EditorAction)row);
        else                             ctx_menu_close(a);
        return;
    }
    if (a->ctx_menu_kind == CTX_KIND_MENU) {
        int idx = a->ctx_menu_target;
        int sn  = menu_count_static(idx);
        int n   = menu_count(idx);
        if (row < 0 || row >= n) { ctx_menu_close(a); return; }
        if (row < sn) {
            void (*fn)(App*) = MENU_TABLES[idx][row].fn;
            ctx_menu_close(a);
            if (fn) fn(a);
            return;
        }
        /* "Recent vaults" submenu row — clicking it just toggles the
         * inner popup; it doesn't close the parent menu. The submenu
         * also opens automatically on hover (see MOUSEMOTION). */
        if (idx == 0 && row == sn && app_recent_dirs_count() > 0) {
            a->ctx_submenu_active = !a->ctx_submenu_active;
            return;
        }
        ctx_menu_close(a);
        return;
    }
    ctx_menu_invoke(a, ctx_action_at_row(a, row));
}

/* ----------------------------- sidebar drag-and-drop -------------------- */

#define DND_DRAG_THRESHOLD_PX 5

/* Identify the drop target folder for cursor (mx, my). Returns:
 *   >= 0 — vault.items index of a folder
 *   -1   — vault root (cursor in sidebar, above any item)
 *   -2   — no valid target (outside sidebar) */
static int dnd_find_drop_target(const App* a, int mx, int my)
{
    if (!a->sidebar_open || mx < 0 || mx >= a->sidebar_w) return -2;
    int idx = sidebar_item_at(a, mx, my);
    if (idx < 0) return -1;     /* above items in sidebar = root */
    const VaultItem* it = &a->vault.items[idx];
    if (it->is_dir) return idx;
    /* File hovered → drop into its parent folder. Find that parent's
     * vault index by stripping the basename and matching. */
    char dir[1024];
    dirname_of(it->path, dir, sizeof dir);
    if (a->vault.dir && strcmp(dir, a->vault.dir) == 0) return -1;
    for (size_t i = 0; i < a->vault.count; ++i) {
        if (!a->vault.items[i].is_dir) continue;
        if (strcmp(a->vault.items[i].path, dir) == 0) return (int)i;
    }
    return -1;
}

/* Perform the queued move. Source = a->vault.items[dnd_source_idx]; target
 * is dnd_drop_target (-1 for root, >=0 for folder). Returns 0 on success. */
static int dnd_finish_move(App* a)
{
    if (a->dnd_source_idx < 0 || a->dnd_source_idx >= (int)a->vault.count)
        return -1;
    const VaultItem* src = &a->vault.items[a->dnd_source_idx];

    const char* tdir = NULL;
    if (a->dnd_drop_target == -1) {
        tdir = a->vault.dir;
    } else if (a->dnd_drop_target >= 0 &&
               a->dnd_drop_target < (int)a->vault.count &&
               a->vault.items[a->dnd_drop_target].is_dir) {
        tdir = a->vault.items[a->dnd_drop_target].path;
    } else {
        return -1;
    }
    if (!tdir) return -1;

    char old_path[1024];
    snprintf(old_path, sizeof old_path, "%s", src->path);
    char new_path[1024];
    snprintf(new_path, sizeof new_path, "%s/%s", tdir,
             vault_basename(src->path));
    if (strcmp(old_path, new_path) == 0) return 0;     /* same dir, no-op */

    if (rename(old_path, new_path) != 0) {
        fprintf(stderr, "dnd move failed: %s -> %s\n", old_path, new_path);
        return -1;
    }
    /* If we moved the open file, update its path. */
    if (a->note_path && strcmp(a->note_path, old_path) == 0) {
        free(a->note_path);
        a->note_path = strdup(new_path);
        update_window_title(a);
    }
    if (a->vault.dir) vault_scan(&a->vault, a->vault.dir);
    if (a->note_path)
        a->vault.selected = vault_index_of(&a->vault, a->note_path);
    char msg[256];
    snprintf(msg, sizeof msg, "moved \"%s\" to %s",
             vault_basename(new_path),
             a->dnd_drop_target == -1 ? "(root)" :
             vault_basename(tdir));
    app_notify(a, msg);
    return 0;
}

/* Reset DnD state to idle. */
static void dnd_reset(App* a)
{
    a->dnd_armed       = false;
    a->dnd_active      = false;
    a->dnd_source_idx  = -1;
    a->dnd_drop_target = -2;
}

/* Render the ghost label that follows the cursor + highlight target row. */
static void render_dnd_ghost(App* a)
{
    if (!a->dnd_active) return;
    if (a->dnd_source_idx < 0 ||
        a->dnd_source_idx >= (int)a->vault.count) return;
    const VaultItem* src = &a->vault.items[a->dnd_source_idx];

    /* Highlight the drop target row in sidebar. */
    if (a->dnd_drop_target >= 0 &&
        a->dnd_drop_target < (int)a->vault.count) {
        /* Locate that index in the visible list to compute its y. */
        for (int row = 0; row < a->sidebar_visible_count; ++row) {
            if (a->sidebar_visible[row] != a->dnd_drop_target) continue;
            int top    = sidebar_items_top(a);
            int item_h = sidebar_item_height(a);
            int y = top + row * item_h - a->sidebar_scroll_y;
            SDL_Rect hr = { 0, y, a->sidebar_w, item_h };
            SDL_SetRenderDrawColor(a->renderer,
                a->fg_link.r, a->fg_link.g, a->fg_link.b, 90);
            SDL_RenderFillRect(a->renderer, &hr);
            SDL_Rect hb = { 0, y, a->sidebar_w, 2 };
            SDL_SetRenderDrawColor(a->renderer,
                a->fg_link.r, a->fg_link.g, a->fg_link.b, 255);
            SDL_RenderFillRect(a->renderer, &hb);
            break;
        }
    } else if (a->dnd_drop_target == -1 && a->sidebar_open) {
        /* Root: draw a thin highlight bar across the sidebar header. */
        SDL_Rect hr = { 0, 0, a->sidebar_w, sidebar_items_top(a) - 4 };
        SDL_SetRenderDrawColor(a->renderer,
            a->fg_link.r, a->fg_link.g, a->fg_link.b, 70);
        SDL_RenderFillRect(a->renderer, &hr);
    }

    /* Ghost label following the cursor. */
    int tw = font_measure(a->font_body, src->name, strlen(src->name)) + 24;
    int th = font_line_height(a->font_body) + 6;
    SDL_Rect gr = { a->dnd_x + 12, a->dnd_y + 8, tw, th };
    SDL_SetRenderDrawColor(a->renderer, 30, 30, 36, 230);
    SDL_RenderFillRect(a->renderer, &gr);
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_link.r, a->fg_link.g, a->fg_link.b, 220);
    SDL_RenderDrawRect(a->renderer, &gr);
    font_draw_line(a->font_body, src->name, strlen(src->name),
                   gr.x + 12, gr.y + font_ascent(a->font_body) + 2,
                   a->fg);
}

/* ----------------------------- help overlay (F1) ------------------------ */

/* Static reference of all keyboard shortcuts grouped by category. Rendered
 * in a centered modal. Add rows here as new actions are wired up. */
static const struct { const char* key; const char* desc; } HELP_ROWS[] = {
    {"# File", NULL},
    {"Ctrl+N",        "New file"},
    {"Ctrl+O",        "Open file…"},
    {"Ctrl+S",        "Save"},
    {"Ctrl+Shift+S",  "Save as…"},
    {"F2",            "Rename"},
    {"Ctrl+P",        "Quick switcher"},
    {"# Edit", NULL},
    {"Ctrl+Z",        "Undo"},
    {"Ctrl+Shift+Z",  "Redo"},
    {"Ctrl+A",        "Select all"},
    {"Ctrl+C/X/V",    "Copy / Cut / Paste"},
    {"# View", NULL},
    {"Ctrl+E",        "Toggle Edit / Preview"},
    {"Ctrl+B",        "Toggle sidebar"},
    {"Ctrl+,",        "Settings (F10 fallback)"},
    {"F1",            "This help overlay"},
    {"# Find", NULL},
    {"Ctrl+F",        "Find"},
    {"Ctrl+H",        "Find & replace"},
    {"Ctrl+Shift+F",  "Search vault (across all notes)"},
    {"Enter",         "Next match  (Shift+Enter previous)"},
    {"Ctrl+Enter",    "Replace one  (Ctrl+Shift+Enter all)"},
    {"Alt+I",         "Toggle case-insensitive  (find)"},
    {"Alt+W",         "Toggle whole-word        (find)"},
    {"Alt+R",         "Toggle regex             (find)"},
    {"# Notes", NULL},
    {"Ctrl+Shift+O",  "Outline overlay (jump by heading)"},
    {"Ctrl+Alt+O",    "Pin outline as right-side panel"},
    {"Ctrl+Shift+B",  "Backlinks (files linking here)"},
    {"Ctrl+Shift+G",  "Tag panel (vault-wide #tag list)"},
    {"Ctrl+Shift+E",  "Export current note to HTML"},
    {"Ctrl+Alt+T",    "Align markdown table at cursor"},
    {"Ctrl+D",        "Today's daily note"},
    {"Ctrl+N",        "New file (template picker if any)"},
    {"# Keybindings", NULL},
    {"Ctrl+K",        "Keybindings overlay (rebind any action)"},
    {"Ctrl+,/F10",    "Settings (theme, fonts, sidebar width…)"},
    {"Ctrl+Shift+T",  "Theme color picker"},
    {"# Markdown", NULL},
    {"[[",            "Wiki-link auto-complete (edit mode)"},
    {"Ctrl+click",    "Follow wiki-link"},
    {"F12 / Ctrl+Enter", "Follow wiki-link at cursor (edit)"},
    {"Click ☐/☑",     "Toggle task (preview)"},
    {"# Sidebar", NULL},
    {"Right-click",   "File context menu (Open / Rename / Delete / New)"},
    {"Drag",          "Drop a file on a folder to move it"},
    {"# Misc", NULL},
    {"Esc",           "Close overlay / exit edit mode"},
    {NULL, NULL},
};

static void help_open (App* a) { a->help_active = true;  a->help_scroll = 0; }
static void help_close(App* a) { a->help_active = false; }

/* Forward decls for the action enumeration referenced in render_help. */
static int         ACTIONS_count(void);
static const char* ACTIONS_name(int i);
static const char* ACTIONS_category(int i);
static const char* current_keystr_for(const char* action);

static void render_help(App* a)
{
    if (!a->help_active) return;

    /* Backdrop. */
    overlay_backdrop(a);

    int rh = font_line_height(a->font_body) + 4;
    int box_w = 580;
    /* Count visible rows to size the box (HELP_ROWS reference + per-action
     * current bindings appended below). */
    int n_rows = 0;
    for (int i = 0; HELP_ROWS[i].key; ++i) n_rows++;
    int an = ACTIONS_count();
    n_rows += an + 2;        /* +2 = "Current bindings" header + spacer */
    int box_h = rh * (n_rows + 3) + 24;     /* +3: title, hint, padding */
    if (box_h > a->win_h - 80) box_h = a->win_h - 80;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = (a->win_h - box_h) / 2;

    SDL_Rect box = { box_x, box_y, box_w, box_h };
    overlay_card(a, box);

    /* Clip rows to the box interior so a long list scrolls cleanly. */
    SDL_Rect clip = { box_x + 4, box_y + rh + 8,
                      box_w - 8, box_h - rh - 28 };
    SDL_RenderSetClipRect(a->renderer, &clip);

    /* Title */
    {
        const char* title = "Keyboard shortcuts";
        font_draw_line(a->font_body, title, strlen(title),
                       box_x + 16, box_y + 10 + font_ascent(a->font_body),
                       a->fg_link);
    }

    int y = box_y + rh + 10 - a->help_scroll;
    int key_x  = box_x + 24;
    int desc_x = box_x + 230;
    for (int i = 0; HELP_ROWS[i].key; ++i) {
        const char* k = HELP_ROWS[i].key;
        const char* d = HELP_ROWS[i].desc;
        if (k[0] == '#') {
            /* Category header. */
            SDL_Color hc = a->fg_heading;
            font_draw_line(a->font_body, k + 2, strlen(k + 2),
                           box_x + 16, y + font_ascent(a->font_body) + 4, hc);
            y += rh + 2;
        } else {
            font_draw_line(a->font_body, k, strlen(k),
                           key_x, y + font_ascent(a->font_body), a->fg_link);
            if (d) font_draw_line(a->font_body, d, strlen(d),
                                  desc_x, y + font_ascent(a->font_body),
                                  a->fg);
            y += rh;
        }
    }

    /* "Current bindings" section: walk ACTIONS, show the actual bound
     * keystroke (user-rebound or default). Reflects rebinds the user
     * may have made via the keybindings overlay. */
    {
        const char* hdr = "Current bindings (rebindable in Ctrl+K)";
        font_draw_line(a->font_body, hdr, strlen(hdr),
                       box_x + 16, y + font_ascent(a->font_body) + 6,
                       a->fg_heading);
        y += rh + 8;
        int actions_n = ACTIONS_count();
        for (int i = 0; i < actions_n; ++i) {
            const char* name = ACTIONS_name(i);
            const char* cat  = ACTIONS_category(i);
            const char* key  = current_keystr_for(name);
            if (!key || !*key) key = "(unbound)";
            char desc[128];
            if (cat && *cat)
                snprintf(desc, sizeof desc, "%s \xc2\xb7 %s", cat, name);
            else
                snprintf(desc, sizeof desc, "%s", name);
            font_draw_line(a->font_body, key, strlen(key),
                           key_x, y + font_ascent(a->font_body), a->fg_link);
            font_draw_line(a->font_body, desc, strlen(desc),
                           desc_x, y + font_ascent(a->font_body), a->fg);
            y += rh;
        }
    }

    SDL_RenderSetClipRect(a->renderer, NULL);

    /* Hint at the bottom. */
    const char* hint = "Esc to close  ·  \xe2\x86\x91/\xe2\x86\x93 scroll";
    font_draw_line(a->font_body, hint, strlen(hint),
                   box_x + 16, box_y + box_h - 8 - rh + font_ascent(a->font_body),
                   a->fg_muted);
}

/* ----------------------------- user keybindings ------------------------- */

/* User-defined keybindings table. Loaded from the cfg's `keybindings` key
 * at startup, mutated by the keybindings overlay, and persisted on save. */
typedef struct { char keystr[40]; char action[32]; } UserKeybind;
static UserKeybind g_user_kbinds[64];
static int         g_user_kbind_count;

/* Find a user-bound keystr; returns the action, or NULL. */
static const char* user_kbind_for(const char* keystr)
{
    for (int i = 0; i < g_user_kbind_count; ++i)
        if (strcmp(g_user_kbinds[i].keystr, keystr) == 0)
            return g_user_kbinds[i].action;
    return NULL;
}

/* Find the keystr currently bound to `action`; returns NULL if none. */
static const char* user_kbind_keystr_for_action(const char* action)
{
    for (int i = 0; i < g_user_kbind_count; ++i)
        if (strcmp(g_user_kbinds[i].action, action) == 0)
            return g_user_kbinds[i].keystr;
    return NULL;
}

/* Set or replace a user keybind. If `keystr` is empty, removes any existing
 * bind for `action`. */
static void user_kbind_set(const char* action, const char* keystr)
{
    /* Drop any existing entry for the same action OR keystr. */
    int wi = 0;
    for (int ri = 0; ri < g_user_kbind_count; ++ri) {
        if (strcmp(g_user_kbinds[ri].action, action) == 0) continue;
        if (keystr && *keystr &&
            strcmp(g_user_kbinds[ri].keystr, keystr) == 0) continue;
        if (wi != ri) g_user_kbinds[wi] = g_user_kbinds[ri];
        wi++;
    }
    g_user_kbind_count = wi;
    if (keystr && *keystr &&
        g_user_kbind_count < (int)(sizeof g_user_kbinds / sizeof g_user_kbinds[0]))
    {
        snprintf(g_user_kbinds[g_user_kbind_count].keystr,
                 sizeof g_user_kbinds[0].keystr, "%s", keystr);
        snprintf(g_user_kbinds[g_user_kbind_count].action,
                 sizeof g_user_kbinds[0].action, "%s", action);
        g_user_kbind_count++;
    }
}

static void user_kbind_load_cb(const char* k, const char* v, void* ud)
{
    (void)ud;
    user_kbind_set(v, k);     /* cfg shape is {keystr = action} */
}

static void user_kbinds_load_from_cfg(LuaHost* h)
{
    g_user_kbind_count = 0;
    lua_host_each_in_table(h, "keybindings", user_kbind_load_cb, NULL);
}

/* ----------------------------- settings page ---------------------------- */

/* Forward decls — definitions live in the picker / keybindings sections below. */
static void keybind_open (App* a);
static void picker_open  (App* a);
static int  save_note_with_eol(App* a, const char* path);

/* Picker color-slot helpers, defined in the picker section below. Needed
 * here because settings_persist writes the color block. */
typedef struct ColorSlot { const char* label; const char* key; } ColorSlot;
#define COLOR_SLOT_COUNT 14
extern const ColorSlot g_color_slots[COLOR_SLOT_COUNT];
static SDL_Color* color_slot_ptr(App* a, int idx);

typedef enum {
    SET_THEME,
    SET_FONT,
    SET_SIZE,
    SET_SIZE_H1,
    SET_SIZE_H2,
    SET_SIZE_H3,
    SET_LINE_SPACING,   /* extra pixels between rendered lines */
    SET_LINE_ENDINGS,   /* preserve / LF / CRLF */
    SET_SIDEBAR_W,
    SET_KEYBINDINGS,    /* opens the keybindings overlay */
    SET_COLORS,         /* opens the color picker overlay */
    SET_CONVERT_LF,     /* one-shot: rewrite current note as LF */
    SET_RESET,          /* delete init-overrides.lua, factory defaults */
    SET_COUNT,
} SettingsRow;

static const char* SETTINGS_LABELS[SET_COUNT] = {
    "Theme",
    "Body font",
    "Font size",
    "H1 size",
    "H2 size",
    "H3 size",
    "Line spacing",
    "Line endings",
    "Sidebar width",
    "Keybindings",
    "Colors",
    "Convert this file to LF",
    "Reset to defaults",
};

/* Format the current value of a settings row into `out`. */
static void settings_value_str(const App* a, SettingsRow r, char* out, size_t cap)
{
    switch (r) {
        case SET_THEME:
            if (a->settings_theme_idx >= 0 &&
                a->settings_theme_idx < G_THEME_COUNT)
                snprintf(out, cap, "%s",
                         g_themes[a->settings_theme_idx].name);
            else
                snprintf(out, cap, "(custom)");
            break;
        case SET_FONT:
            if (a->settings_font_idx >= 0 &&
                a->settings_font_idx < g_font_choice_count)
                snprintf(out, cap, "%s",
                         g_font_choices[a->settings_font_idx].name);
            else
                snprintf(out, cap, "%s", a->cfg_font_path);
            break;
        case SET_SIZE:        snprintf(out, cap, "%d", a->cfg_font_size);    break;
        case SET_SIZE_H1:     snprintf(out, cap, "%d", a->cfg_font_size_h1); break;
        case SET_SIZE_H2:     snprintf(out, cap, "%d", a->cfg_font_size_h2); break;
        case SET_SIZE_H3:     snprintf(out, cap, "%d", a->cfg_font_size_h3); break;
        case SET_LINE_SPACING:snprintf(out, cap, "+%d", a->cfg_line_spacing); break;
        case SET_LINE_ENDINGS:
            snprintf(out, cap, "%s",
                     a->cfg_line_endings == 1 ? "LF (Unix)" :
                     a->cfg_line_endings == 2 ? "CRLF (Windows)" :
                                                "Preserve");
            break;
        case SET_CONVERT_LF:  snprintf(out, cap, "(Enter)");                  break;
        case SET_SIDEBAR_W:   snprintf(out, cap, "%d", a->sidebar_w);        break;
        case SET_KEYBINDINGS: snprintf(out, cap, "(Enter)");                 break;
        case SET_COLORS:      snprintf(out, cap, "(Enter)");                 break;
        case SET_RESET:       snprintf(out, cap, "(Enter)");                 break;
        default:              snprintf(out, cap, "?"); break;
    }
}

/* Adjust the focused setting by `dir` (+1 = right, -1 = left). Applies the
 * change immediately so the user sees the effect. */
static void settings_adjust(App* a, SettingsRow r, int dir)
{
    bool need_reload_fonts = false;
    switch (r) {
        case SET_THEME: {
            int n = G_THEME_COUNT;
            if (n == 0) break;
            a->settings_theme_idx = (a->settings_theme_idx + dir + n) % n;
            theme_apply(a, a->settings_theme_idx);
            break;
        }
        case SET_FONT: {
            if (g_font_choice_count == 0) break;
            int n = g_font_choice_count;
            a->settings_font_idx = (a->settings_font_idx + dir + n) % n;
            snprintf(a->cfg_font_path, sizeof a->cfg_font_path,
                     "%s", g_font_choices[a->settings_font_idx].path);
            snprintf(a->cfg_font_path_mono, sizeof a->cfg_font_path_mono,
                     "%s", g_font_choices[a->settings_font_idx].path);
            need_reload_fonts = true;
            break;
        }
        case SET_SIZE: {
            int v = a->cfg_font_size + dir;
            if (v < 8)  v = 8;
            if (v > 40) v = 40;
            if (v != a->cfg_font_size) { a->cfg_font_size = v; need_reload_fonts = true; }
            break;
        }
        case SET_SIZE_H1: {
            int v = a->cfg_font_size_h1 + dir;
            if (v < 12) v = 12;
            if (v > 64) v = 64;
            if (v != a->cfg_font_size_h1) { a->cfg_font_size_h1 = v; need_reload_fonts = true; }
            break;
        }
        case SET_SIZE_H2: {
            int v = a->cfg_font_size_h2 + dir;
            if (v < 10) v = 10;
            if (v > 56) v = 56;
            if (v != a->cfg_font_size_h2) { a->cfg_font_size_h2 = v; need_reload_fonts = true; }
            break;
        }
        case SET_SIZE_H3: {
            int v = a->cfg_font_size_h3 + dir;
            if (v < 10) v = 10;
            if (v > 48) v = 48;
            if (v != a->cfg_font_size_h3) { a->cfg_font_size_h3 = v; need_reload_fonts = true; }
            break;
        }
        case SET_LINE_SPACING: {
            int v = a->cfg_line_spacing + dir;
            if (v < 0)  v = 0;
            if (v > 24) v = 24;
            a->cfg_line_spacing = v;
            /* Re-clamp scroll because doc height changed instantly. */
            if (a->edit_mode) ensure_cursor_visible(a);
            break;
        }
        case SET_LINE_ENDINGS: {
            int v = (a->cfg_line_endings + dir + 3) % 3;
            a->cfg_line_endings = v;
            break;
        }
        case SET_CONVERT_LF: {
            /* One-shot: rewrite the open file as LF on disk + scrub the
             * in-memory buffer's CRs so subsequent edits don't re-introduce
             * CRLFs. Only applies when there's a real saved file. */
            if (!a->note_path || strcmp(a->note_path, "(unsaved)") == 0 ||
                strcmp(a->note_path, "(welcome)") == 0)
            {
                app_notify(a, "save the file first");
                break;
            }
            int prev = a->cfg_line_endings;
            a->cfg_line_endings = 1;     /* force LF */
            if (save_note_with_eol(a, a->note_path) == 0) {
                /* Reload to scrub any \r chars from the buffer. */
                load_note(a, a->note_path);
                app_notify(a, "converted to LF");
            } else {
                app_notify(a, "save failed");
            }
            a->cfg_line_endings = prev;
            break;
        }
        case SET_SIDEBAR_W: {
            int step = 10;
            int v = a->sidebar_w + dir * step;
            if (v < 120) v = 120;
            if (v > a->win_w / 2) v = a->win_w / 2;
            a->sidebar_w = v;
            break;
        }
        case SET_KEYBINDINGS: {
            keybind_open(a);
            break;
        }
        case SET_COLORS: {
            picker_open(a);
            break;
        }
        case SET_RESET: {
            /* Restore the bundled defaults (overrides any previous changes
             * AND removes the saved override file). User must re-customise
             * to bring back any preferences. */
            snprintf(a->cfg_font_path, sizeof a->cfg_font_path,
                     "C:/Windows/Fonts/consola.ttf");
            snprintf(a->cfg_font_path_mono, sizeof a->cfg_font_path_mono,
                     "C:/Windows/Fonts/consola.ttf");
            a->cfg_font_size    = 16;
            a->cfg_font_size_h1 = 28;
            a->cfg_font_size_h2 = 22;
            a->cfg_font_size_h3 = 18;
            a->sidebar_w        = 240;
            a->settings_theme_idx = 0;
            theme_apply(a, 0);
            int idx = font_choice_find(a->cfg_font_path);
            if (idx >= 0) a->settings_font_idx = idx;
            need_reload_fonts = true;
            remove("settings.lua");
            app_notify(a, "settings reset to defaults");
            break;
        }
        default: break;
    }
    if (need_reload_fonts) app_reload_fonts(a);
}

/* Write a Lua string literal — handles `"` and `\` escapes (Windows paths). */
static void fputs_lua_string(FILE* f, const char* s)
{
    fputc('"', f);
    for (const char* p = s; *p; ++p) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

/* Persist the current live settings to data/init-overrides.lua. Loaded after
 * init.lua at next startup, with overlay semantics (overlay keys win). */
/* Trigger settings_persist after the vault is changed at runtime. Defined
 * here so it sits next to the persistence routine; just forwards. */
static int settings_persist(App* a);
static void persist_vault_path(App* a) { settings_persist(a); }

/* Push `dir` onto recent_dirs (most-recent first), removing any prior copy
 * and capping the list at 5 entries. Quiet no-op for empty paths. */
static void recent_dirs_push(App* a, const char* dir)
{
    if (!dir || !*dir) return;
    int max = (int)(sizeof a->recent_dirs / sizeof a->recent_dirs[0]);
    /* Remove an existing entry so it floats to the top. */
    int found = -1;
    for (int i = 0; i < a->recent_dirs_count; ++i) {
        if (strcmp(a->recent_dirs[i], dir) == 0) { found = i; break; }
    }
    if (found >= 0) {
        for (int i = found; i + 1 < a->recent_dirs_count; ++i)
            snprintf(a->recent_dirs[i], sizeof a->recent_dirs[i],
                     "%s", a->recent_dirs[i + 1]);
        a->recent_dirs_count--;
    }
    /* Shift everyone down by one and put new entry at index 0. */
    int n = a->recent_dirs_count < max - 1 ? a->recent_dirs_count : max - 1;
    for (int i = n; i > 0; --i)
        snprintf(a->recent_dirs[i], sizeof a->recent_dirs[i],
                 "%s", a->recent_dirs[i - 1]);
    snprintf(a->recent_dirs[0], sizeof a->recent_dirs[0], "%s", dir);
    if (a->recent_dirs_count < max) a->recent_dirs_count++;
}

static void recent_dirs_load(App* a)
{
    a->recent_dirs_count = 0;
    int n = lua_host_cfg_array_length(a->lua, "recent_dirs");
    int max = (int)(sizeof a->recent_dirs / sizeof a->recent_dirs[0]);
    if (n > max) n = max;
    for (int i = 0; i < n; ++i) {
        const char* s = lua_host_cfg_array_string(a->lua, "recent_dirs", i + 1);
        /* Skip empty strings and the COMPUTER sentinel — neither is a real
         * directory and we never want to surface either to the user. */
        if (!s || !*s) continue;
        if (strcmp(s, "::COMPUTER::") == 0) continue;
        snprintf(a->recent_dirs[a->recent_dirs_count],
                 sizeof a->recent_dirs[0], "%s", s);
        a->recent_dirs_count++;
    }
}

static int settings_persist(App* a)
{
    const char* path = "settings.lua";
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f,
        "-- Downsee settings -- auto-generated.\n"
        "-- Edit values to override defaults. Re-saving from the\n"
        "-- settings page (Ctrl+,) overwrites this file with the\n"
        "-- known keys; manual additions outside that set survive\n"
        "-- only until the next save.\n"
        "return {\n");
    fprintf(f, "    font_path      = "); fputs_lua_string(f, a->cfg_font_path);      fprintf(f, ",\n");
    fprintf(f, "    font_path_mono = "); fputs_lua_string(f, a->cfg_font_path_mono); fprintf(f, ",\n");
    fprintf(f, "    font_size      = %d,\n", a->cfg_font_size);
    fprintf(f, "    font_size_h1   = %d,\n", a->cfg_font_size_h1);
    fprintf(f, "    font_size_h2   = %d,\n", a->cfg_font_size_h2);
    fprintf(f, "    font_size_h3   = %d,\n", a->cfg_font_size_h3);
    fprintf(f, "    line_spacing   = %d,\n", a->cfg_line_spacing);
    fprintf(f, "    line_endings   = %d,  -- 0=preserve 1=LF 2=CRLF\n",
            a->cfg_line_endings);
    fprintf(f, "    sidebar_width  = %d,\n", a->sidebar_w);
    fprintf(f, "    outline_pinned = %s,\n",
            a->outline_pinned ? "true" : "false");
    fprintf(f, "    outline_panel_width = %d,\n", a->outline_panel_w);
    if (a->vault.dir && a->vault.dir[0]) {
        fprintf(f, "    vault_path     = ");
        fputs_lua_string(f, a->vault.dir);
        fprintf(f, ",\n");
    }
    /* Recent vault directories — most-recent first, capped at 5. */
    if (a->recent_dirs_count > 0) {
        fprintf(f, "    recent_dirs    = {\n");
        for (int i = 0; i < a->recent_dirs_count; ++i) {
            fprintf(f, "        ");
            fputs_lua_string(f, a->recent_dirs[i]);
            fprintf(f, ",\n");
        }
        fprintf(f, "    },\n");
    }
    if (a->settings_theme_idx >= 0 && a->settings_theme_idx < G_THEME_COUNT) {
        /* A named theme is selected. Write only the theme name; let it
         * define the 14 colors at next launch. Writing color_* here would
         * pin those values forever and the theme name would be cosmetic. */
        fprintf(f, "    theme          = ");
        fputs_lua_string(f, g_themes[a->settings_theme_idx].name);
        fprintf(f, ",\n");
    } else {
        /* Custom palette (set via the color picker). Write every color so
         * we can restore the user's exact tweaks at next launch. */
        for (int i = 0; i < COLOR_SLOT_COUNT; ++i) {
            SDL_Color* c = color_slot_ptr(a, i);
            if (!c) continue;
            fprintf(f, "    %-18s = { %3d, %3d, %3d, %3d },\n",
                    g_color_slots[i].key, c->r, c->g, c->b, c->a);
        }
    }
    if (g_user_kbind_count > 0) {
        fprintf(f, "    keybindings    = {\n");
        for (int i = 0; i < g_user_kbind_count; ++i) {
            fprintf(f, "        [");
            fputs_lua_string(f, g_user_kbinds[i].keystr);
            fprintf(f, "] = ");
            fputs_lua_string(f, g_user_kbinds[i].action);
            fprintf(f, ",\n");
        }
        fprintf(f, "    },\n");
    }
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}

static void settings_open (App* a)
{
    a->settings_active   = true;
    a->settings_selected = 0;
    a->settings_hover    = -1;
    /* Re-sync the font index in case cfg_font_path was changed elsewhere. */
    int i = font_choice_find(a->cfg_font_path);
    if (i >= 0) a->settings_font_idx = i;
}

static void settings_close(App* a)
{
    if (!a->settings_active) return;
    a->settings_active = false;
    if (settings_persist(a) == 0)
        app_notify(a, "settings saved (data/init-overrides.lua)");
}

/* Settings layout — dynamic, derived from font metrics so the panel
 * doesn't break at larger font sizes (where labels overran the value
 * column at fixed pixel offsets). */
#define SETTINGS_BOX_Y    80

/* Width of the widest label, in pixels. */
static int settings_label_w(const App* a)
{
    int wmax = 0;
    for (int r = 0; r < SET_COUNT; ++r) {
        int w = font_measure(a->font_body, SETTINGS_LABELS[r],
                             strlen(SETTINGS_LABELS[r]));
        if (w > wmax) wmax = w;
    }
    return wmax;
}

/* Forward decl — settings_value_str writes into `out`. */
static void settings_value_str(const App* a, SettingsRow r, char* out, size_t cap);

/* Width of the widest VALUE string, so the box never clips long font
 * names, theme names, or the like. */
static int settings_value_w(const App* a)
{
    int wmax = 0;
    char val[260];
    for (int r = 0; r < SET_COUNT; ++r) {
        settings_value_str(a, (SettingsRow)r, val, sizeof val);
        int w = font_measure(a->font_body, val, strlen(val));
        if (w > wmax) wmax = w;
    }
    return wmax;
}

/* Geometry helpers, all derived from font metrics + label/value widths. */
static int settings_chev_sz   (const App* a) { return font_line_height(a->font_body) + 8; }
static int settings_box_w     (const App* a)
{
    /* Layout: 24 (left pad) + label + 32 (gap) + chev + 12 (gap)
     *       + value + 32 (gap) + chev + 24 (right pad).
     * The gaps are intentionally wide so that long values like "Cascadia
     * Mono" still have visible separation from the chevrons. */
    int chev = settings_chev_sz(a);
    int w = 24 + settings_label_w(a) + 32 + chev + 12
          + settings_value_w(a) + 32 + chev + 24;
    if (w < 560) w = 560;
    if (w > a->win_w - 40) w = a->win_w - 40;
    return w;
}
static int settings_val_x_off (const App* a)
{
    return 24 + settings_label_w(a) + 32 + settings_chev_sz(a) + 12;
}
static int settings_box_x     (const App* a) { return (a->win_w - settings_box_w(a)) / 2; }

/* Hit-test the settings overlay: which row (and what part) is at (mx, my)?
 * Returns:
 *   row    >= 0  if cursor is inside a row; -1 if outside the rows
 *   *part        — 'L' for the left chevron / left half, 'R' for the right
 *                  chevron / right half, 'B' for the row body (label/value).
 *   inside_box   — set to true if the cursor is inside the box (so the
 *                  caller knows whether a click should close the overlay). */
static int settings_hit_test(const App* a, int mx, int my,
                             char* part, bool* inside_box)
{
    int row_h = font_line_height(a->font_body) + 8;
    int box_w = settings_box_w(a);
    int box_h = row_h * (SET_COUNT + 2) + 28;
    int box_x = settings_box_x(a);
    int box_y = SETTINGS_BOX_Y;
    if (inside_box)
        *inside_box = (mx >= box_x && mx < box_x + box_w &&
                       my >= box_y && my < box_y + box_h);
    int rows_top = box_y + 10 + row_h;
    if (mx < box_x + 4 || mx >= box_x + box_w - 4 ||
        my < rows_top || my >= rows_top + row_h * SET_COUNT) return -1;
    int r = (my - rows_top) / row_h;
    if (r < 0 || r >= SET_COUNT) return -1;
    int val_x = box_x + settings_val_x_off(a);
    int chev  = settings_chev_sz(a);
    int left_chev_x  = val_x - chev - 4;
    int right_chev_x = box_x + box_w - chev - 8;
    if (part) {
        if      (mx >= right_chev_x && mx < right_chev_x + chev) *part = 'R';
        else if (mx >= left_chev_x  && mx < left_chev_x  + chev) *part = 'L';
        else                                                     *part = 'B';
    }
    return r;
}

static void render_settings(App* a)
{
    if (!a->settings_active) return;

    overlay_backdrop(a);

    int row_h = font_line_height(a->font_body) + 8;
    int box_w = settings_box_w(a);
    int box_h = row_h * (SET_COUNT + 2) + 28;       /* +2 = title + hint */
    int box_x = settings_box_x(a);
    int box_y = SETTINGS_BOX_Y;
    int chev  = settings_chev_sz(a);

    SDL_Rect box = { box_x, box_y, box_w, box_h };
    overlay_card(a, box);

    int y = box_y + 10;
    /* Title */
    {
        const char* title = "Settings";
        font_draw_line(a->font_body, title, strlen(title),
                       box_x + 16, y + font_ascent(a->font_body), a->fg_link);
    }
    y += row_h;
    SDL_Rect div = { box_x + 8, y - 2, box_w - 16, 1 };
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 100);
    SDL_RenderFillRect(a->renderer, &div);

    int val_x = box_x + settings_val_x_off(a);

    for (int r = 0; r < SET_COUNT; ++r) {
        bool sel   = (r == a->settings_selected);
        bool hover = (r == a->settings_hover) && !sel;
        if (sel || hover) {
            SDL_Rect hr = { box_x + 4, y, box_w - 8, row_h };
            SDL_Color bc = sel ? a->bg_sidebar_active : a->bg_sidebar_hover;
            SDL_SetRenderDrawColor(a->renderer, bc.r, bc.g, bc.b, sel ? 255 : 200);
            SDL_RenderFillRect(a->renderer, &hr);
        }
        SDL_Color label_c = sel ? a->fg : a->fg_muted;
        font_draw_line(a->font_body, SETTINGS_LABELS[r],
                       strlen(SETTINGS_LABELS[r]),
                       box_x + 16, y + font_ascent(a->font_body) + 2,
                       label_c);

        /* Value column. Chevrons drawn procedurally so they always render —
         * but only for rows where left/right does anything. "(Enter)" rows
         * (Keybindings, Colors, Convert, Reset) don't have a value to cycle
         * through, so showing chevrons would be misleading. */
        char val[260];
        settings_value_str(a, (SettingsRow)r, val, sizeof val);
        bool is_enter_row = (r == SET_KEYBINDINGS || r == SET_COLORS ||
                             r == SET_CONVERT_LF  || r == SET_RESET);
        if (sel && !is_enter_row) {
            int left_chev_x  = val_x - chev - 4;
            int right_chev_x = box_x + box_w - chev - 8;
            icon_draw(a->renderer, ICON_CHEVRON_LEFT,
                      left_chev_x,  y + (row_h - chev) / 2, chev, a->fg_link);
            icon_draw(a->renderer, ICON_CHEVRON_RIGHT,
                      right_chev_x, y + (row_h - chev) / 2, chev, a->fg_link);
        }
        SDL_Color val_c = sel ? a->fg_link : a->fg;
        font_draw_line(a->font_body, val, strlen(val),
                       val_x, y + font_ascent(a->font_body) + 2, val_c);
        y += row_h;
    }

    /* Hint line — plain text so glyph fallback isn't needed. */
    y += 4;
    const char* hint = "Up/Dn navigate  -  Left/Right change  -  Esc save & close";
    font_draw_line(a->font_body, hint, strlen(hint),
                   box_x + 16, y + font_ascent(a->font_body), a->fg_muted);
}

/* ----------------------------- color picker ----------------------------- */

#define PICKER_BOX_W   600
#define PICKER_BOX_Y    60

/* `ColorSlot` and `COLOR_SLOT_COUNT` are forward-declared up in the settings
 * section so settings_persist can write the color block. */

const ColorSlot g_color_slots[COLOR_SLOT_COUNT] = {
    { "Background",      "color_bg"             },
    { "Text",            "color_fg"             },
    { "Heading",         "color_heading"        },
    { "Quote",           "color_quote"          },
    { "Link",            "color_link"           },
    { "Code BG",         "color_code_bg"        },
    { "Muted",           "color_muted"          },
    { "Sidebar BG",      "color_sidebar_bg"     },
    { "Sidebar hover",   "color_sidebar_hover"  },
    { "Sidebar active",  "color_sidebar_active" },
    { "Status BG",       "color_status_bg"      },
    { "Status FG",       "color_status_fg"      },
    { "Selection",       "color_selection"      },
    { "Cursor",          "color_cursor"         },
};
/* Compile-time check that COLOR_SLOT_COUNT matches g_color_slots length. */
typedef char color_slot_count_check[
    (COLOR_SLOT_COUNT == (int)(sizeof g_color_slots / sizeof g_color_slots[0]))
    ? 1 : -1];

/* Map a slot index to the SDL_Color field on the App (so the picker can
 * mutate the running theme in-place). Order must match g_color_slots. */
static SDL_Color* color_slot_ptr(App* a, int idx)
{
    switch (idx) {
        case  0: return &a->bg;
        case  1: return &a->fg;
        case  2: return &a->fg_heading;
        case  3: return &a->fg_quote;
        case  4: return &a->fg_link;
        case  5: return &a->bg_code;
        case  6: return &a->fg_muted;
        case  7: return &a->bg_sidebar;
        case  8: return &a->bg_sidebar_hover;
        case  9: return &a->bg_sidebar_active;
        case 10: return &a->bg_status;
        case 11: return &a->fg_status;
        case 12: return &a->bg_selection;
        case 13: return &a->fg_cursor;
    }
    return NULL;
}

static void picker_open(App* a)
{
    a->picker_active   = true;
    a->picker_selected = 0;
    a->picker_hover    = -1;
    a->picker_channel  = 0;
    a->picker_scroll   = 0;
    a->settings_active = false;     /* close settings while in picker */
    a->keybind_active  = false;
}

/* Forward decl; defined below in settings_persist update. */
static int settings_persist(App* a);

static void picker_close(App* a)
{
    if (!a->picker_active) return;
    a->picker_active = false;
    if (settings_persist(a) == 0)
        app_notify(a, "colors saved (data/init-overrides.lua)");
}

/* Adjust the focused channel of the focused slot by `delta`, clamped to
 * 0..255. Marks the live theme as "custom" so subsequent persistence
 * doesn't write a stale theme name that would overwrite the colors at
 * the next launch. */
static void picker_adjust(App* a, int delta)
{
    if (a->picker_selected < 0 ||
        a->picker_selected >= COLOR_SLOT_COUNT) return;
    SDL_Color* c = color_slot_ptr(a, a->picker_selected);
    if (!c) return;
    unsigned char* ch = (a->picker_channel == 0) ? &c->r
                      : (a->picker_channel == 1) ? &c->g
                      : (a->picker_channel == 2) ? &c->b
                      :                            &c->a;
    int v = (int)*ch + delta;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    *ch = (unsigned char)v;
    /* Mark as custom so the persisted theme name doesn't shadow on reload. */
    a->settings_theme_idx = -1;
}

static int picker_row_h(const App* a) { return font_line_height(a->font_body) + 8; }

static int picker_scrollbar_geom(const App* a,
                                 SDL_Rect* track, SDL_Rect* thumb)
{
    int rh    = picker_row_h(a);
    int box_w = PICKER_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = PICKER_BOX_Y;
    int max_box_h = a->win_h - 80;
    int box_h = rh * (COLOR_SLOT_COUNT + 3) + 24;
    if (box_h > max_box_h) box_h = max_box_h;
    int rows_top = box_y + rh + 12;
    int rows_bot = box_y + box_h - rh - 8;
    return overlay_list_scrollbar_geom(box_x, box_w, rows_top, rows_bot,
                                       rh * COLOR_SLOT_COUNT,
                                       a->picker_scroll, track, thumb);
}

static int picker_hit_test(const App* a, int mx, int my,
                           int* out_channel, bool* inside_box)
{
    int rh    = picker_row_h(a);
    int box_w = PICKER_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = PICKER_BOX_Y;
    int max_box_h = a->win_h - 80;
    int box_h = rh * (COLOR_SLOT_COUNT + 3) + 24;
    if (box_h > max_box_h) box_h = max_box_h;
    if (inside_box)
        *inside_box = (mx >= box_x && mx < box_x + box_w &&
                       my >= box_y && my < box_y + box_h);

    int rows_top = box_y + rh + 12;
    int rows_bot = box_y + box_h - rh - 8;
    if (mx < box_x + 8 || mx >= box_x + box_w - 8) return -1;
    if (my < rows_top || my >= rows_bot) return -1;
    int r = (my - rows_top + a->picker_scroll) / rh;
    if (r < 0 || r >= COLOR_SLOT_COUNT) return -1;

    /* Channel hit: the four cells "R: NNN  G: NNN  B: NNN  A: NNN" sit at
     * x = box_x + 220, each cell ~60 px wide. */
    if (out_channel) {
        int chx = mx - (box_x + 220);
        int idx = chx / 60;
        if (idx >= 0 && idx < 4) *out_channel = idx;
        else                     *out_channel = -1;
    }
    return r;
}

static void render_picker(App* a)
{
    if (!a->picker_active) return;

    overlay_backdrop(a);

    int rh    = picker_row_h(a);
    int box_w = PICKER_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = PICKER_BOX_Y;
    int max_box_h = a->win_h - 80;
    int box_h = rh * (COLOR_SLOT_COUNT + 3) + 24;
    if (box_h > max_box_h) box_h = max_box_h;

    SDL_Rect box = { box_x, box_y, box_w, box_h };
    overlay_card(a, box);

    /* Title */
    {
        const char* title = "Theme colors";
        font_draw_line(a->font_body, title, strlen(title),
                       box_x + 16, box_y + 10 + font_ascent(a->font_body),
                       a->fg_link);
    }

    int rows_top = box_y + rh + 12;
    int rows_bot = box_y + box_h - rh - 8;
    SDL_Rect clip = { box_x + 4, rows_top - 4,
                      box_w - 8, rows_bot - rows_top + 4 };
    SDL_RenderSetClipRect(a->renderer, &clip);

    int y = rows_top - a->picker_scroll;
    for (int i = 0; i < COLOR_SLOT_COUNT; ++i, y += rh) {
        if (y + rh < rows_top || y > rows_bot) continue;
        bool sel = (i == a->picker_selected);
        bool hov = (i == a->picker_hover) && !sel;
        if (sel || hov) {
            SDL_Rect hr = { box_x + 4, y, box_w - 8, rh };
            SDL_Color bc = sel ? a->bg_sidebar_active : a->bg_sidebar_hover;
            SDL_SetRenderDrawColor(a->renderer, bc.r, bc.g, bc.b, 255);
            SDL_RenderFillRect(a->renderer, &hr);
        }

        /* Color swatch. Outline so transparent / very dark fills stay visible. */
        SDL_Color* c = color_slot_ptr(a, i);
        if (c) {
            SDL_Rect sw = { box_x + 16, y + 4, 18, rh - 8 };
            SDL_SetRenderDrawColor(a->renderer, c->r, c->g, c->b, 255);
            SDL_RenderFillRect(a->renderer, &sw);
            SDL_SetRenderDrawColor(a->renderer,
                a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 255);
            SDL_RenderDrawRect(a->renderer, &sw);
        }

        /* Label */
        const char* lab = g_color_slots[i].label;
        SDL_Color lab_c = sel ? a->fg : a->fg_muted;
        font_draw_line(a->font_body, lab, strlen(lab),
                       box_x + 44, y + font_ascent(a->font_body) + 2,
                       lab_c);

        /* Channels: "R: 24  G: 24  B: 28  A: 255". The active channel of the
         * selected row is highlighted (link color). */
        if (c) {
            const char* names = "RGBA";
            int comps[4] = { c->r, c->g, c->b, c->a };
            int cx = box_x + 220;
            for (int k = 0; k < 4; ++k) {
                char cell[16];
                snprintf(cell, sizeof cell, "%c: %3d", names[k], comps[k]);
                bool active_chan = sel && (k == a->picker_channel);
                SDL_Color cc = active_chan ? a->fg_link : a->fg;
                font_draw_line(a->font_body, cell, strlen(cell),
                               cx, y + font_ascent(a->font_body) + 2, cc);
                cx += 60;
            }
        }
    }
    SDL_RenderSetClipRect(a->renderer, NULL);

    SDL_Rect sb_track, sb_thumb;
    if (picker_scrollbar_geom(a, &sb_track, &sb_thumb))
        overlay_scrollbar_draw(a, &sb_track, &sb_thumb,
                               a->sb_drag == SB_PICKER);

    /* Hint at the bottom. The `\xb1` escapes are split with adjacent string
     * literals so the next ASCII digit isn't read as part of the hex escape. */
    const char* hint =
        "Tab channel  \xc2\xb7  \xc2\xb1" "5 / Shift \xc2\xb1" "1  "
        "\xc2\xb7  Wheel adjust  \xc2\xb7  Esc closes";
    font_draw_line(a->font_body, hint, strlen(hint),
                   box_x + 16,
                   box_y + box_h - 8 - rh + font_ascent(a->font_body),
                   a->fg_muted);
}

/* ----------------------------- outline (TOC) ---------------------------- */

/* Forward decls — definitions live further down in the file, but both the
 * outline and vault-search overlays activate hits by switching into edit
 * mode and scrolling the cursor into view. */
static void enter_edit_mode      (App* a);
static void ensure_cursor_visible(App* a);
static void search_rebuild       (App* a);

#define OUTLINE_BOX_W   520
#define OUTLINE_BOX_Y    60

static int outline_row_h(const App* a) { return font_line_height(a->font_body) + 6; }

/* Walk the buffer and collect every `#`/`##`/... heading into outline_entries.
 * Lines starting with 1-6 hashes followed by a space qualify (matches the
 * existing render_editor heading detection). */
static void outline_collect(App* a)
{
    a->outline_count = 0;
    Buffer* b = &a->buf;
    size_t n_lines = buffer_line_count(b);
    for (size_t li = 0; li < n_lines; ++li) {
        size_t ls = buffer_line_start(b, li);
        size_t le = buffer_line_end(b, li);
        size_t llen = le - ls;
        if (llen == 0) continue;
        const char* s = b->data + ls;
        int level = 0;
        while (level < 6 && level < (int)llen && s[level] == '#') level++;
        if (level == 0 || level >= (int)llen || s[level] != ' ') continue;

        if (a->outline_count >= a->outline_cap) {
            a->outline_cap = a->outline_cap ? a->outline_cap * 2 : 32;
            a->outline_entries = realloc(a->outline_entries,
                a->outline_cap * sizeof(*a->outline_entries));
        }
        struct OutlineEntry* o = &a->outline_entries[a->outline_count++];
        o->line_no = (int)li;
        o->level   = level;
        size_t text_off = (size_t)level + 1;
        size_t tlen = (llen > text_off) ? (llen - text_off) : 0;
        if (tlen > sizeof o->text - 1) tlen = sizeof o->text - 1;
        memcpy(o->text, s + text_off, tlen);
        o->text[tlen] = 0;
        if (tlen > 0 && o->text[tlen - 1] == '\r') o->text[tlen - 1] = 0;
    }
}

static void outline_open(App* a)
{
    outline_collect(a);
    a->outline_active   = true;
    a->outline_selected = a->outline_count > 0 ? 0 : -1;
    a->outline_hover    = -1;
    a->outline_scroll   = 0;
}

static void outline_close(App* a)
{
    a->outline_active = false;
}

static int outline_hit_test(const App* a, int mx, int my)
{
    int rh    = outline_row_h(a);
    int box_w = OUTLINE_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = OUTLINE_BOX_Y;
    int max_box_h = a->win_h - 80;
    int box_h = rh * (a->outline_count + 3) + 24;
    if (box_h > max_box_h) box_h = max_box_h;
    int rows_top = box_y + rh + 12;
    int rows_bot = box_y + box_h - rh - 8;
    if (mx < box_x + 8 || mx >= box_x + box_w - 8) return -1;
    if (my < rows_top || my >= rows_bot) return -1;
    int r = (my - rows_top + a->outline_scroll) / rh;
    if (r < 0 || r >= a->outline_count) return -1;
    return r;
}

static int outline_scrollbar_geom(const App* a,
                                  SDL_Rect* track, SDL_Rect* thumb)
{
    int rh    = outline_row_h(a);
    int box_w = OUTLINE_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = OUTLINE_BOX_Y;
    int max_box_h = a->win_h - 80;
    int box_h = rh * (a->outline_count + 3) + 24;
    if (box_h > max_box_h) box_h = max_box_h;
    int rows_top = box_y + rh + 12;
    int rows_bot = box_y + box_h - rh - 8;
    return overlay_list_scrollbar_geom(box_x, box_w, rows_top, rows_bot,
                                       rh * a->outline_count,
                                       a->outline_scroll, track, thumb);
}

/* Move cursor to the start of the heading line and switch to edit mode. */
static void outline_activate(App* a)
{
    if (a->outline_selected < 0 ||
        a->outline_selected >= a->outline_count) return;
    struct OutlineEntry* o = &a->outline_entries[a->outline_selected];
    Buffer* b = &a->buf;
    size_t target = (size_t)o->line_no;
    if (target >= buffer_line_count(b)) target = buffer_line_count(b) - 1;
    size_t ls = buffer_line_start(b, target);
    b->cursor     = ls;
    b->sel_anchor = (long)ls;
    if (!a->edit_mode) enter_edit_mode(a);
    ensure_cursor_visible(a);
    bump_blink(a);
    outline_close(a);
}

static void render_outline(App* a)
{
    if (!a->outline_active) return;

    overlay_backdrop(a);

    int rh    = outline_row_h(a);
    int box_w = OUTLINE_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = OUTLINE_BOX_Y;
    int max_box_h = a->win_h - 80;
    int box_h = rh * (a->outline_count + 3) + 24;
    if (box_h > max_box_h) box_h = max_box_h;

    SDL_Rect box = { box_x, box_y, box_w, box_h };
    overlay_card(a, box);

    /* Title with count. */
    char title[80];
    snprintf(title, sizeof title, "Outline  (%d heading%s)",
             a->outline_count, a->outline_count == 1 ? "" : "s");
    font_draw_line(a->font_body, title, strlen(title),
                   box_x + 16, box_y + 10 + font_ascent(a->font_body),
                   a->fg_link);

    int rows_top = box_y + rh + 12;
    int rows_bot = box_y + box_h - rh - 8;
    SDL_Rect clip = { box_x + 4, rows_top - 2,
                      box_w - 8, rows_bot - rows_top + 4 };
    SDL_RenderSetClipRect(a->renderer, &clip);

    int y = rows_top - a->outline_scroll;
    for (int i = 0; i < a->outline_count; ++i, y += rh) {
        if (y + rh < rows_top || y > rows_bot) continue;
        struct OutlineEntry* o = &a->outline_entries[i];
        bool sel = (i == a->outline_selected);
        bool hov = (i == a->outline_hover) && !sel;
        if (sel || hov) {
            SDL_Rect hr = { box_x + 4, y, box_w - 8, rh };
            SDL_Color bc = sel ? a->bg_sidebar_active : a->bg_sidebar_hover;
            SDL_SetRenderDrawColor(a->renderer, bc.r, bc.g, bc.b, 255);
            SDL_RenderFillRect(a->renderer, &hr);
        }
        int indent = (o->level - 1) * 16;
        SDL_Color tc = sel ? a->fg : (o->level == 1 ? a->fg_heading : a->fg_muted);
        font_draw_line(a->font_body, o->text, strlen(o->text),
                       box_x + 16 + indent,
                       row_text_baseline(a->font_body, y, rh),
                       tc);
    }

    if (a->outline_count == 0) {
        const char* empty = "(no headings in this note)";
        font_draw_line(a->font_body, empty, strlen(empty),
                       box_x + 16,
                       rows_top + font_ascent(a->font_body),
                       a->fg_muted);
    }
    SDL_RenderSetClipRect(a->renderer, NULL);

    SDL_Rect sb_track, sb_thumb;
    if (outline_scrollbar_geom(a, &sb_track, &sb_thumb))
        overlay_scrollbar_draw(a, &sb_track, &sb_thumb,
                               a->sb_drag == SB_OUTLINE_LIST);

    /* Hint. */
    const char* hint = "\xe2\x86\x91/\xe2\x86\x93 navigate  \xc2\xb7  "
        "Enter jump  \xc2\xb7  Esc close";
    font_draw_line(a->font_body, hint, strlen(hint),
                   box_x + 16,
                   box_y + box_h - 8 - rh + font_ascent(a->font_body),
                   a->fg_muted);
}

/* ----------------------------- outline pinned panel --------------------- */

/* Find the index of the heading whose line is the closest <= cursor's line,
 * so we can highlight the section the user is currently in. -1 if none. */
static int outline_index_of_cursor(const App* a)
{
    if (a->outline_count == 0) return -1;
    if (!a->edit_mode) return -1;
    size_t line, col;
    buffer_cursor_pos((Buffer*)&a->buf, &line, &col);
    int best = -1;
    for (int i = 0; i < a->outline_count; ++i) {
        if ((int)line >= a->outline_entries[i].line_no) best = i;
        else break;
    }
    return best;
}

static int outline_panel_x(const App* a) { return a->win_w - a->outline_panel_w; }

static int outline_panel_row_h(const App* a) {
    return font_line_height(a->font_body) + 8;
}

/* Hit-test the pinned panel: returns outline entry index, or -1. */
static int outline_panel_hit_test(const App* a, int mx, int my)
{
    if (!a->outline_pinned) return -1;
    int px = outline_panel_x(a);
    if (mx < px || mx >= a->win_w) return -1;
    int top = chrome_bar_h(a) + font_line_height(a->font_body) + 18;
    int bot = a->win_h - status_bar_h(a);
    if (my < top || my >= bot) return -1;
    int rh = outline_panel_row_h(a);
    int r = (my - top + a->outline_scroll) / rh;
    if (r < 0 || r >= a->outline_count) return -1;
    return r;
}

/* Activate a panel row: jump cursor + ensure visible. Doesn't close the
 * panel since it's pinned. */
static void outline_panel_activate(App* a, int idx)
{
    if (idx < 0 || idx >= a->outline_count) return;
    struct OutlineEntry* o = &a->outline_entries[idx];
    Buffer* b = &a->buf;
    size_t target = (size_t)o->line_no;
    if (target >= buffer_line_count(b)) target = buffer_line_count(b) - 1;
    size_t ls = buffer_line_start(b, target);
    b->cursor     = ls;
    b->sel_anchor = (long)ls;
    if (!a->edit_mode) enter_edit_mode(a);
    ensure_cursor_visible(a);
    bump_blink(a);
}

static void render_outline_panel(App* a)
{
    if (!a->outline_pinned) return;

    /* Live-rebuild every frame: cheap (~1 µs per 100 lines) and means the
     * panel reflects edits instantly without us tracking buffer changes. */
    outline_collect(a);
    int cursor_idx = outline_index_of_cursor(a);

    int px = outline_panel_x(a);
    int pw = a->outline_panel_w;
    int top = chrome_bar_h(a);
    int bot = a->win_h - status_bar_h(a);
    int ph  = bot - top;

    /* Panel bg + left hairline divider. */
    SDL_Rect bg = { px, top, pw, ph };
    SDL_SetRenderDrawColor(a->renderer,
        a->bg_sidebar.r, a->bg_sidebar.g, a->bg_sidebar.b, 255);
    SDL_RenderFillRect(a->renderer, &bg);
    SDL_Rect div = { px, top, 1, ph };
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 60);
    SDL_RenderFillRect(a->renderer, &div);

    /* Header. */
    int header_y = top + 8;
    char header[80];
    snprintf(header, sizeof header, "Outline  \xc2\xb7  %d",
             a->outline_count);
    font_draw_line(a->font_body, header, strlen(header),
                   px + 12, header_y + font_ascent(a->font_body),
                   a->fg);

    int rows_top = top + font_line_height(a->font_body) + 18;
    /* Hairline divider under the header so it reads as a section, not as
     * just another row in the list. */
    SDL_Rect hdiv = { px + 12, rows_top - 6, pw - 24, 1 };
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 70);
    SDL_RenderFillRect(a->renderer, &hdiv);

    SDL_Rect clip = { px + 1, rows_top - 2, pw - 1, bot - rows_top };
    SDL_RenderSetClipRect(a->renderer, &clip);

    int rh = outline_panel_row_h(a);
    int y = rows_top - a->outline_scroll;
    for (int i = 0; i < a->outline_count; ++i, y += rh) {
        if (y + rh < rows_top || y > bot) continue;
        struct OutlineEntry* o = &a->outline_entries[i];
        bool sel = (i == cursor_idx);
        bool hov = (i == a->outline_hover) && !sel;
        if (sel) {
            SDL_Rect tint = { px + 1, y, pw - 1, rh };
            SDL_SetRenderDrawColor(a->renderer,
                a->bg_sidebar_hover.r, a->bg_sidebar_hover.g,
                a->bg_sidebar_hover.b, 200);
            SDL_RenderFillRect(a->renderer, &tint);
            SDL_Rect bar = { px + 1, y, 3, rh };
            SDL_SetRenderDrawColor(a->renderer,
                a->fg_link.r, a->fg_link.g, a->fg_link.b, 255);
            SDL_RenderFillRect(a->renderer, &bar);
        } else if (hov) {
            SDL_Rect r = { px + 1, y, pw - 1, rh };
            SDL_SetRenderDrawColor(a->renderer,
                a->bg_sidebar_hover.r, a->bg_sidebar_hover.g,
                a->bg_sidebar_hover.b, 130);
            SDL_RenderFillRect(a->renderer, &r);
        }
        int indent = (o->level - 1) * 12;
        SDL_Color tc = sel        ? a->fg
                     : (o->level == 1) ? a->fg
                                       : a->fg_muted;
        /* Truncate text to fit. Brutal byte-wise truncation; good enough
         * for ASCII headings. */
        const char* text = o->text;
        size_t tlen = strlen(text);
        int avail_w = pw - 16 - indent - 8;
        if (avail_w < 30) avail_w = 30;
        while (tlen > 0 &&
               font_measure(a->font_body, text, tlen) > avail_w) tlen--;
        font_draw_line(a->font_body, text, tlen,
                       px + 12 + indent, y + font_ascent(a->font_body) + 2,
                       tc);
    }

    if (a->outline_count == 0) {
        const char* empty = "(no headings)";
        font_draw_line(a->font_body, empty, strlen(empty),
                       px + 12,
                       rows_top + font_ascent(a->font_body),
                       a->fg_muted);
    }
    SDL_RenderSetClipRect(a->renderer, NULL);
}

static void action_outline_pin(App* a)
{
    a->outline_pinned = !a->outline_pinned;
    a->outline_scroll = 0;
    if (a->outline_pinned) outline_collect(a);
    /* Re-clamp document scroll in case pinning shrank the viewport such
     * that the user is now scrolled past the bottom. */
    clamp_scroll(a);
}

/* ----------------------------- vault search ----------------------------- */

#define VSEARCH_BOX_W      720
#define VSEARCH_BOX_Y       60
#define VSEARCH_PREVIEW_X  130     /* x of the preview text within a hit row */

/* Forward decl — load_note is at the top of the file already, this just
 * keeps the call site readable. enter_edit_mode / ensure_cursor_visible
 * are forward-declared at the top of the outline section above. */
static int  load_note(App* a, const char* path);

static int  vsearch_row_h(const App* a) { return font_line_height(a->font_body) + 6; }
static int  vsearch_input_h(const App* a) { return font_line_height(a->font_body) + 12; }

/* Append a row to the hits buffer, growing as needed. The struct is
 * defined inline in app.h; size is captured here at call sites. */
static struct VSearchHit* vsearch_append_row(App* a)
{
    if (a->vsearch_count >= a->vsearch_cap) {
        a->vsearch_cap = a->vsearch_cap ? a->vsearch_cap * 2 : 64;
        a->vsearch_hits = realloc(a->vsearch_hits,
                                  a->vsearch_cap * sizeof(*a->vsearch_hits));
    }
    return &a->vsearch_hits[a->vsearch_count++];
}

/* Same case-insensitive byte equality the local find uses. */
static int vsearch_byte_eq(unsigned char a, unsigned char b, int ci)
{
    if (!ci) return a == b;
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
    return a == b;
}

/* Find the next literal-mode match in `data[from..len)`. Sets *out_start +
 * *out_end and returns 1 on hit; 0 if no more. */
static int vsearch_literal_next(const char* q, size_t qlen, int ci,
                                const char* data, size_t len, size_t from,
                                size_t* out_start, size_t* out_end)
{
    if (qlen == 0 || from + qlen > len) return 0;
    for (size_t i = from; i + qlen <= len; ++i) {
        size_t k;
        for (k = 0; k < qlen; ++k)
            if (!vsearch_byte_eq((unsigned char)data[i + k],
                                 (unsigned char)q[k], ci)) break;
        if (k == qlen) { *out_start = i; *out_end = i + qlen; return 1; }
    }
    return 0;
}

static void vsearch_clear(App* a)
{
    a->vsearch_count            = 0;
    a->vsearch_selected         = -1;
    a->vsearch_files_with_hits  = 0;
    a->vsearch_total_hits       = 0;
    a->vsearch_re_err[0]        = 0;
}

/* Run the query across every .md file in the vault and rebuild hit rows.
 * For each file with at least one hit, emit a header row + up to 50 hit
 * rows. Stops globally at 1000 hit rows to bound memory. */
static void vsearch_rebuild(App* a)
{
    vsearch_clear(a);
    if (a->vsearch_qlen == 0) return;

    DsRegex* re = NULL;
    if (a->vsearch_regex) {
        re = ds_regex_compile(a->vsearch_query, a->vsearch_ci,
                              a->vsearch_re_err, sizeof a->vsearch_re_err);
        if (!re) return;
    }

    const int kMaxRows         = 1000;
    const int kMaxHitsPerFile  = 50;

    for (size_t vi = 0; vi < a->vault.count; ++vi) {
        VaultItem* it = &a->vault.items[vi];
        if (it->is_dir) continue;
        size_t nlen = strlen(it->name);
        if (nlen < 4 || strcmp(it->name + nlen - 3, ".md") != 0) continue;

        size_t flen = 0;
        char*  data = slurp(it->path, &flen);
        if (!data) continue;

        int file_hits = 0;
        size_t cursor = 0;
        while (cursor <= flen && a->vsearch_count < kMaxRows &&
               file_hits < kMaxHitsPerFile)
        {
            size_t s, e;
            int ok;
            if (re) ok = ds_regex_find(re, data, flen, cursor, &s, &e);
            else    ok = vsearch_literal_next(a->vsearch_query, a->vsearch_qlen,
                                              a->vsearch_ci, data, flen, cursor,
                                              &s, &e);
            if (!ok) break;

            /* Find the line bounds and 1-based line number of `s`. */
            size_t ls = s, le = e;
            int    line_no = 1;
            for (size_t i = 0; i < s; ++i)
                if (data[i] == '\n') line_no++;
            while (ls > 0 && data[ls - 1] != '\n') ls--;
            while (le < flen && data[le] != '\n') le++;

            /* On the first hit in this file, emit a header row. */
            if (file_hits == 0) {
                struct VSearchHit* hr = vsearch_append_row(a);
                hr->vault_idx = (int)vi;
                hr->line_no   = 0;
                hr->match_col_in_line = 0;
                hr->match_len         = 0;
                snprintf(hr->preview, sizeof hr->preview, "%s", it->name);
                a->vsearch_files_with_hits++;
            }

            /* Build the preview slice. If the line is longer than the buffer,
             * window it around the match so the hit stays visible. */
            size_t llen = le - ls;
            int    match_col = (int)(s - ls);
            int    pcap = (int)sizeof ((struct VSearchHit*)0)->preview - 1;
            int    win_start = 0;
            if ((int)llen > pcap) {
                win_start = match_col - 20;
                if (win_start < 0) win_start = 0;
                if (win_start + pcap > (int)llen) win_start = (int)llen - pcap;
                if (win_start < 0) win_start = 0;
            }
            int win_len = (int)llen - win_start;
            if (win_len > pcap) win_len = pcap;

            struct VSearchHit* h = vsearch_append_row(a);
            h->vault_idx        = (int)vi;
            h->line_no          = line_no;
            h->match_col_in_line = match_col - win_start;
            h->match_len         = (int)(e - s);
            /* Clamp the highlighted span to the preview window. */
            if (h->match_col_in_line < 0) {
                h->match_len += h->match_col_in_line;
                h->match_col_in_line = 0;
            }
            if (h->match_col_in_line + h->match_len > win_len)
                h->match_len = win_len - h->match_col_in_line;
            if (h->match_len < 0) h->match_len = 0;

            memcpy(h->preview, data + ls + win_start, win_len);
            h->preview[win_len] = 0;
            /* Strip any trailing CR for clean display. */
            if (win_len > 0 && h->preview[win_len - 1] == '\r')
                h->preview[win_len - 1] = 0;

            file_hits++;
            a->vsearch_total_hits++;

            /* Advance past this match (avoid zero-length infinite loop). */
            cursor = (e == s) ? s + 1 : e;
        }
        free(data);
        if (a->vsearch_count >= kMaxRows) break;
    }

    if (re) ds_regex_free(re);

    /* Auto-select the first hit row (skipping the leading header). */
    a->vsearch_selected = -1;
    for (int i = 0; i < a->vsearch_count; ++i) {
        if (a->vsearch_hits[i].line_no > 0) { a->vsearch_selected = i; break; }
    }
}

static void vsearch_open(App* a)
{
    a->vsearch_active   = true;
    a->vsearch_hover    = -1;
    a->vsearch_scroll   = 0;
    /* Don't clobber a previous query — let the user re-run by Enter. */
    if (!SDL_IsTextInputActive()) SDL_StartTextInput();
}

static void vsearch_close(App* a)
{
    a->vsearch_active = false;
}

/* Move selection by `dir` over hit rows, skipping headers. Wraps at ends. */
static void vsearch_move(App* a, int dir)
{
    if (a->vsearch_count == 0) return;
    int n = a->vsearch_count;
    int s = a->vsearch_selected;
    for (int step = 0; step < n; ++step) {
        s = (s + dir + n) % n;
        if (a->vsearch_hits[s].line_no > 0) {
            a->vsearch_selected = s;
            return;
        }
    }
}

/* Open the file under the selected hit and scroll to the matching line. */
static void vsearch_activate(App* a)
{
    if (a->vsearch_selected < 0 ||
        a->vsearch_selected >= a->vsearch_count) return;
    struct VSearchHit* h = &a->vsearch_hits[a->vsearch_selected];
    if (h->vault_idx < 0 || h->vault_idx >= (int)a->vault.count) return;
    if (h->line_no <= 0) return;
    if (!confirm_discard(a)) return;
    load_note(a, a->vault.items[h->vault_idx].path);

    /* Move cursor to the start of the matched line and switch to edit mode
     * so the user lands on the hit. line_no is 1-based. */
    Buffer* b = &a->buf;
    size_t target_line = (size_t)h->line_no - 1;
    if (target_line >= buffer_line_count(b))
        target_line = buffer_line_count(b) - 1;
    size_t ls = buffer_line_start(b, target_line);
    b->cursor     = ls + (size_t)h->match_col_in_line;
    if (b->cursor > b->len) b->cursor = b->len;
    b->sel_anchor = (long)b->cursor;
    if (h->match_len > 0)
        b->cursor = b->cursor + (size_t)h->match_len;
    if (b->cursor > b->len) b->cursor = b->len;

    if (!a->edit_mode) enter_edit_mode(a);
    ensure_cursor_visible(a);
    bump_blink(a);
    vsearch_close(a);
}

/* Hit-test: returns row index under (mx, my), or -1. */
static int vsearch_hit_test(const App* a, int mx, int my)
{
    int rh    = vsearch_row_h(a);
    int box_w = VSEARCH_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = VSEARCH_BOX_Y;
    int max_box_h = a->win_h - 80;
    int box_h = vsearch_input_h(a) + rh * (a->vsearch_count + 2) + 24;
    if (box_h > max_box_h) box_h = max_box_h;
    int rows_top = box_y + vsearch_input_h(a) + 8;
    int rows_bot = box_y + box_h - rh - 8;
    if (mx < box_x + 8 || mx >= box_x + box_w - 8) return -1;
    if (my < rows_top || my >= rows_bot) return -1;
    int r = (my - rows_top + a->vsearch_scroll) / rh;
    if (r < 0 || r >= a->vsearch_count) return -1;
    return r;
}

static int vsearch_scrollbar_geom(const App* a,
                                  SDL_Rect* track, SDL_Rect* thumb)
{
    int rh    = vsearch_row_h(a);
    int box_w = VSEARCH_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = VSEARCH_BOX_Y;
    int max_box_h = a->win_h - 80;
    int box_h = vsearch_input_h(a) + rh * (a->vsearch_count + 2) + 24;
    if (box_h > max_box_h) box_h = max_box_h;
    int rows_top = box_y + vsearch_input_h(a) + 8;
    int rows_bot = box_y + box_h - rh - 8;
    return overlay_list_scrollbar_geom(box_x, box_w, rows_top, rows_bot,
                                       rh * a->vsearch_count,
                                       a->vsearch_scroll, track, thumb);
}

static void render_vsearch(App* a)
{
    if (!a->vsearch_active) return;

    overlay_backdrop(a);

    int rh    = vsearch_row_h(a);
    int ih    = vsearch_input_h(a);
    int box_w = VSEARCH_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = VSEARCH_BOX_Y;
    int max_box_h = a->win_h - 80;
    int box_h = ih + rh * (a->vsearch_count + 2) + 24;
    if (box_h > max_box_h) box_h = max_box_h;

    SDL_Rect box = { box_x, box_y, box_w, box_h };
    overlay_card(a, box);

    /* Input row at the top: "Search vault:  <query>             [Re] [Aa] N hits in M files" */
    int y = box_y + 8;
    const char* lab = "Search vault: ";
    int lab_w = font_measure(a->font_body, lab, strlen(lab));
    font_draw_line(a->font_body, lab, strlen(lab),
                   box_x + 16, y + font_ascent(a->font_body), a->fg_link);
    font_draw_line(a->font_body, a->vsearch_query, a->vsearch_qlen,
                   box_x + 16 + lab_w, y + font_ascent(a->font_body), a->fg);

    /* Mode indicators. */
    int ind_x = box_x + box_w - 16;
    {
        const char* r_lab = "[Re]";
        int rw = font_measure(a->font_body, r_lab, 4);
        ind_x -= rw + 6;
        SDL_Color rc = a->vsearch_regex ? a->fg_link : a->fg_muted;
        if (a->vsearch_regex && a->vsearch_re_err[0])
            rc = (SDL_Color){ 230, 110, 110, 255 };
        font_draw_line(a->font_body, r_lab, 4, ind_x,
                       y + font_ascent(a->font_body), rc);
    }
    {
        const char* i_lab = "[Aa]";
        int iw = font_measure(a->font_body, i_lab, 4);
        ind_x -= iw + 6;
        SDL_Color ic = a->vsearch_ci ? a->fg_link : a->fg_muted;
        font_draw_line(a->font_body, i_lab, 4, ind_x,
                       y + font_ascent(a->font_body), ic);
    }

    /* Counts (or error). */
    char counts[96];
    if (a->vsearch_regex && a->vsearch_re_err[0])
        snprintf(counts, sizeof counts, "regex: %s", a->vsearch_re_err);
    else if (a->vsearch_qlen == 0)
        snprintf(counts, sizeof counts, "type to search vault");
    else if (a->vsearch_total_hits == 0)
        snprintf(counts, sizeof counts, "0 hits");
    else
        snprintf(counts, sizeof counts, "%d hits in %d files",
                 a->vsearch_total_hits, a->vsearch_files_with_hits);
    int cw = font_measure(a->font_body, counts, strlen(counts));
    ind_x -= cw + 12;
    SDL_Color cc = (a->vsearch_regex && a->vsearch_re_err[0])
                   ? (SDL_Color){ 230, 110, 110, 255 } : a->fg_muted;
    font_draw_line(a->font_body, counts, strlen(counts),
                   ind_x, y + font_ascent(a->font_body), cc);

    /* Divider below the input. */
    SDL_Rect div = { box_x + 8, box_y + ih, box_w - 16, 1 };
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 100);
    SDL_RenderFillRect(a->renderer, &div);

    /* Result rows. */
    int rows_top = box_y + ih + 8;
    int rows_bot = box_y + box_h - rh - 8;
    SDL_Rect clip = { box_x + 4, rows_top - 2,
                      box_w - 8, rows_bot - rows_top + 4 };
    SDL_RenderSetClipRect(a->renderer, &clip);

    int ry = rows_top - a->vsearch_scroll;
    for (int i = 0; i < a->vsearch_count; ++i, ry += rh) {
        if (ry + rh < rows_top || ry > rows_bot) continue;
        struct VSearchHit* h = &a->vsearch_hits[i];

        if (h->line_no == 0) {
            /* File header: filename in heading color, no selection. */
            font_draw_line(a->font_body, "\xe2\x96\xb8", 3,    /* ▸ */
                           box_x + 12, row_text_baseline(a->font_body, ry, rh),
                           a->fg_muted);
            font_draw_line(a->font_body, h->preview, strlen(h->preview),
                           box_x + 28, row_text_baseline(a->font_body, ry, rh),
                           a->fg_heading);
            continue;
        }

        bool sel = (i == a->vsearch_selected);
        bool hov = (i == a->vsearch_hover) && !sel;
        if (sel || hov) {
            SDL_Rect hr = { box_x + 4, ry, box_w - 8, rh };
            SDL_Color bc = sel ? a->bg_sidebar_active : a->bg_sidebar_hover;
            SDL_SetRenderDrawColor(a->renderer, bc.r, bc.g, bc.b, 255);
            SDL_RenderFillRect(a->renderer, &hr);
        }
        char ln[20];
        snprintf(ln, sizeof ln, "L%d", h->line_no);
        font_draw_line(a->font_body, ln, strlen(ln),
                       box_x + 36, row_text_baseline(a->font_body, ry, rh),
                       a->fg_muted);

        /* Preview text. Highlight the matched run with the selection color
         * before drawing the text on top. */
        size_t plen = strlen(h->preview);
        if (h->match_len > 0 &&
            h->match_col_in_line >= 0 &&
            (size_t)(h->match_col_in_line + h->match_len) <= plen)
        {
            int x0 = font_measure(a->font_body, h->preview,
                                  h->match_col_in_line);
            int x1 = font_measure(a->font_body, h->preview,
                                  h->match_col_in_line + h->match_len);
            SDL_Rect mr = { box_x + VSEARCH_PREVIEW_X + x0,
                            ry + 2, x1 - x0, rh - 4 };
            SDL_SetRenderDrawColor(a->renderer, 220, 160, 60, 140);
            SDL_RenderFillRect(a->renderer, &mr);
        }
        SDL_Color tc = sel ? a->fg : a->fg_muted;
        font_draw_line(a->font_body, h->preview, plen,
                       box_x + VSEARCH_PREVIEW_X,
                       row_text_baseline(a->font_body, ry, rh), tc);
    }
    SDL_RenderSetClipRect(a->renderer, NULL);

    SDL_Rect sb_track, sb_thumb;
    if (vsearch_scrollbar_geom(a, &sb_track, &sb_thumb))
        overlay_scrollbar_draw(a, &sb_track, &sb_thumb,
                               a->sb_drag == SB_VSEARCH);

    /* Hint at the bottom. */
    const char* hint = "\xe2\x86\x91/\xe2\x86\x93 navigate  \xc2\xb7  "
        "Enter open  \xc2\xb7  Alt+R regex  \xc2\xb7  Alt+I case  \xc2\xb7  Esc close";
    font_draw_line(a->font_body, hint, strlen(hint),
                   box_x + 16,
                   box_y + box_h - 8 - rh + font_ascent(a->font_body),
                   a->fg_muted);
}

/* ----------------------------- backlinks panel -------------------------- */

#define BLINK_BOX_W      640
#define BLINK_BOX_Y       60

static int blink_row_h(const App* a) { return font_line_height(a->font_body) + 6; }

/* Lowercase ASCII compare of two strings, NUL-terminated. */
static int ascii_iequal(const char* a, const char* b)
{
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

/* basename(path) without the .md extension, written into out (null-term). */
static void path_basename_no_md(const char* path, char* out, size_t cap)
{
    const char* base = vault_basename(path);
    size_t n = strlen(base);
    if (n >= 3) {
        char tail[4] = { base[n-3], base[n-2], base[n-1], 0 };
        if (ascii_iequal(tail, ".md")) n -= 3;
    }
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = 0;
}

/* Append a hit row, growing as needed. */
static struct BacklinkHit* blink_append(App* a)
{
    if (a->backlinks_count >= a->backlinks_cap) {
        a->backlinks_cap = a->backlinks_cap ? a->backlinks_cap * 2 : 32;
        a->backlinks_hits = realloc(a->backlinks_hits,
                                    a->backlinks_cap * sizeof(*a->backlinks_hits));
    }
    return &a->backlinks_hits[a->backlinks_count++];
}

/* Search every .md file in the vault for `[[<self>]]` (case-insensitive name
 * match). The current note is excluded. Each hit captures filename + the
 * surrounding line, sliced to fit the preview width. */
static void backlinks_collect(App* a)
{
    a->backlinks_count = 0;
    if (!a->note_path || !*a->note_path) return;
    char self[256];
    path_basename_no_md(a->note_path, self, sizeof self);
    if (!*self) return;
    size_t self_len = strlen(self);

    for (size_t vi = 0; vi < a->vault.count; ++vi) {
        VaultItem* it = &a->vault.items[vi];
        if (it->is_dir) continue;
        if (a->note_path && strcmp(it->path, a->note_path) == 0) continue;
        size_t nlen = strlen(it->name);
        if (nlen < 4 || strcmp(it->name + nlen - 3, ".md") != 0) continue;

        size_t flen = 0;
        char*  data = slurp(it->path, &flen);
        if (!data) continue;

        size_t i = 0;
        while (i + 4 + self_len <= flen) {
            if (data[i] != '[' || data[i + 1] != '[') { i++; continue; }
            size_t name_start = i + 2;
            size_t end = name_start;
            while (end + 1 < flen &&
                   !(data[end] == ']' && data[end + 1] == ']') &&
                   data[end] != '\n')
                end++;
            if (end + 1 >= flen || data[end] != ']') { i++; continue; }
            size_t name_len = end - name_start;
            if (name_len == self_len) {
                /* Case-insensitive compare against `self`. */
                size_t k;
                for (k = 0; k < name_len; ++k) {
                    char ca = data[name_start + k], cb = self[k];
                    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
                    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
                    if (ca != cb) break;
                }
                if (k == name_len) {
                    /* Build the hit. Find the line bounds + 1-based number. */
                    size_t ls = i, le = end + 2;
                    int line_no = 1;
                    for (size_t p = 0; p < i; ++p)
                        if (data[p] == '\n') line_no++;
                    while (ls > 0 && data[ls - 1] != '\n') ls--;
                    while (le < flen && data[le] != '\n') le++;

                    struct BacklinkHit* h = blink_append(a);
                    h->vault_idx = (int)vi;
                    h->line_no   = line_no;
                    int pcap = (int)sizeof h->preview - 1;
                    int llen = (int)(le - ls);
                    if (llen > pcap) llen = pcap;
                    memcpy(h->preview, data + ls, llen);
                    h->preview[llen] = 0;
                    if (llen > 0 && h->preview[llen - 1] == '\r')
                        h->preview[llen - 1] = 0;
                }
            }
            i = end + 2;
        }
        free(data);
    }
}

static void backlinks_open(App* a)
{
    backlinks_collect(a);
    a->backlinks_active   = true;
    a->backlinks_selected = a->backlinks_count > 0 ? 0 : -1;
    a->backlinks_hover    = -1;
    a->backlinks_scroll   = 0;
}

static void backlinks_close(App* a) { a->backlinks_active = false; }

static int blink_hit_test(const App* a, int mx, int my)
{
    int rh    = blink_row_h(a);
    int box_w = BLINK_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = BLINK_BOX_Y;
    int max_box_h = a->win_h - 80;
    int box_h = rh * (a->backlinks_count + 3) + 24;
    if (box_h > max_box_h) box_h = max_box_h;
    int rows_top = box_y + rh + 12;
    int rows_bot = box_y + box_h - rh - 8;
    if (mx < box_x + 8 || mx >= box_x + box_w - 8) return -1;
    if (my < rows_top || my >= rows_bot) return -1;
    int r = (my - rows_top + a->backlinks_scroll) / rh;
    if (r < 0 || r >= a->backlinks_count) return -1;
    return r;
}

static int backlinks_scrollbar_geom(const App* a,
                                    SDL_Rect* track, SDL_Rect* thumb)
{
    int rh    = blink_row_h(a);
    int box_w = BLINK_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = BLINK_BOX_Y;
    int max_box_h = a->win_h - 80;
    int box_h = rh * (a->backlinks_count + 3) + 24;
    if (box_h > max_box_h) box_h = max_box_h;
    int rows_top = box_y + rh + 12;
    int rows_bot = box_y + box_h - rh - 8;
    return overlay_list_scrollbar_geom(box_x, box_w, rows_top, rows_bot,
                                       rh * a->backlinks_count,
                                       a->backlinks_scroll, track, thumb);
}

static void backlinks_activate(App* a)
{
    if (a->backlinks_selected < 0 ||
        a->backlinks_selected >= a->backlinks_count) return;
    struct BacklinkHit* h = &a->backlinks_hits[a->backlinks_selected];
    if (h->vault_idx < 0 || h->vault_idx >= (int)a->vault.count) return;
    if (!confirm_discard(a)) return;
    load_note(a, a->vault.items[h->vault_idx].path);
    Buffer* b = &a->buf;
    size_t target = (size_t)h->line_no - 1;
    if (target >= buffer_line_count(b)) target = buffer_line_count(b) - 1;
    size_t ls = buffer_line_start(b, target);
    b->cursor     = ls;
    b->sel_anchor = (long)ls;
    if (!a->edit_mode) enter_edit_mode(a);
    ensure_cursor_visible(a);
    bump_blink(a);
    backlinks_close(a);
}

static void render_backlinks(App* a)
{
    if (!a->backlinks_active) return;

    overlay_backdrop(a);

    int rh    = blink_row_h(a);
    int box_w = BLINK_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = BLINK_BOX_Y;
    int max_box_h = a->win_h - 80;
    int box_h = rh * (a->backlinks_count + 3) + 24;
    if (box_h > max_box_h) box_h = max_box_h;

    SDL_Rect box = { box_x, box_y, box_w, box_h };
    overlay_card(a, box);

    char title[160];
    char self[120];
    if (a->note_path) path_basename_no_md(a->note_path, self, sizeof self);
    else              snprintf(self, sizeof self, "(unsaved)");
    snprintf(title, sizeof title, "Backlinks to [[%s]]  (%d)",
             self, a->backlinks_count);
    font_draw_line(a->font_body, title, strlen(title),
                   box_x + 16, box_y + 10 + font_ascent(a->font_body),
                   a->fg_link);

    int rows_top = box_y + rh + 12;
    int rows_bot = box_y + box_h - rh - 8;
    SDL_Rect clip = { box_x + 4, rows_top - 2,
                      box_w - 8, rows_bot - rows_top + 4 };
    SDL_RenderSetClipRect(a->renderer, &clip);

    int y = rows_top - a->backlinks_scroll;
    for (int i = 0; i < a->backlinks_count; ++i, y += rh) {
        if (y + rh < rows_top || y > rows_bot) continue;
        struct BacklinkHit* h = &a->backlinks_hits[i];
        bool sel = (i == a->backlinks_selected);
        bool hov = (i == a->backlinks_hover) && !sel;
        if (sel || hov) {
            SDL_Rect hr = { box_x + 4, y, box_w - 8, rh };
            SDL_Color bc = sel ? a->bg_sidebar_active : a->bg_sidebar_hover;
            SDL_SetRenderDrawColor(a->renderer, bc.r, bc.g, bc.b, 255);
            SDL_RenderFillRect(a->renderer, &hr);
        }
        const char* fname = (h->vault_idx >= 0 &&
                             h->vault_idx < (int)a->vault.count)
                            ? a->vault.items[h->vault_idx].name : "?";
        char prefix[256];
        snprintf(prefix, sizeof prefix, "%s : L%d", fname, h->line_no);
        int pw = font_measure(a->font_body, prefix, strlen(prefix));
        font_draw_line(a->font_body, prefix, strlen(prefix),
                       box_x + 16, row_text_baseline(a->font_body, y, rh),
                       sel ? a->fg_link : a->fg_heading);
        font_draw_line(a->font_body, h->preview, strlen(h->preview),
                       box_x + 16 + pw + 16,
                       row_text_baseline(a->font_body, y, rh),
                       sel ? a->fg : a->fg_muted);
    }

    if (a->backlinks_count == 0) {
        const char* empty = "(no backlinks found)";
        font_draw_line(a->font_body, empty, strlen(empty),
                       box_x + 16,
                       rows_top + font_ascent(a->font_body),
                       a->fg_muted);
    }
    SDL_RenderSetClipRect(a->renderer, NULL);

    SDL_Rect sb_track, sb_thumb;
    if (backlinks_scrollbar_geom(a, &sb_track, &sb_thumb))
        overlay_scrollbar_draw(a, &sb_track, &sb_thumb,
                               a->sb_drag == SB_BACKLINKS);

    const char* hint = "\xe2\x86\x91/\xe2\x86\x93 navigate  \xc2\xb7  Enter open  \xc2\xb7  Esc close";
    font_draw_line(a->font_body, hint, strlen(hint),
                   box_x + 16,
                   box_y + box_h - 8 - rh + font_ascent(a->font_body),
                   a->fg_muted);
}

/* ----------------------------- tag panel -------------------------------- */

#define TAGS_BOX_W   460
#define TAGS_BOX_Y    60

static int tags_row_h(const App* a) { return font_line_height(a->font_body) + 6; }

/* Same boundary rules as compute_edit_styles: # at start-of-line or after
 * whitespace/`(`/`[`, then at least one word char. Tag body extends through
 * word chars + `-` and `/`. */
static int tag_is_boundary(unsigned char prev)
{
    return prev == 0 || prev == ' ' || prev == '\t' || prev == '\n' ||
           prev == '\r' || prev == '(' || prev == '[';
}

static int tag_is_body_byte(unsigned char c)
{
    return is_word_char(c) || c == '-' || c == '/';
}

/* Find or insert a tag entry; returns the entry pointer. */
static struct TagEntry* tags_intern(App* a, const char* name, size_t nlen)
{
    if (nlen >= sizeof(((struct TagEntry*)0)->name)) nlen = sizeof(((struct TagEntry*)0)->name) - 1;
    for (int i = 0; i < a->tags_count; ++i) {
        if (strncmp(a->tags_entries[i].name, name, nlen) == 0 &&
            a->tags_entries[i].name[nlen] == 0)
            return &a->tags_entries[i];
    }
    if (a->tags_count >= a->tags_cap) {
        a->tags_cap = a->tags_cap ? a->tags_cap * 2 : 32;
        a->tags_entries = realloc(a->tags_entries,
                                  a->tags_cap * sizeof(*a->tags_entries));
    }
    struct TagEntry* t = &a->tags_entries[a->tags_count++];
    memcpy(t->name, name, nlen);
    t->name[nlen] = 0;
    t->count = 0;
    return t;
}

static int tags_compare(const void* a, const void* b)
{
    const struct TagEntry* x = (const struct TagEntry*)a;
    const struct TagEntry* y = (const struct TagEntry*)b;
    if (x->count != y->count) return y->count - x->count;     /* desc by count */
    return strcmp(x->name, y->name);
}

/* Trampoline so fm_each_tag can hand tags off to the global App* picker. */
static void tags_fm_cb(const char* t, size_t n, void* ud)
{
    App* a = (App*)ud;
    tags_intern(a, t, n)->count++;
}

static void tags_collect(App* a)
{
    a->tags_count = 0;
    for (size_t vi = 0; vi < a->vault.count; ++vi) {
        VaultItem* it = &a->vault.items[vi];
        if (it->is_dir) continue;
        size_t nlen = strlen(it->name);
        if (nlen < 4 || strcmp(it->name + nlen - 3, ".md") != 0) continue;

        size_t flen = 0;
        char*  data = slurp(it->path, &flen);
        if (!data) continue;

        /* Pull tags from this file's YAML frontmatter (if any), then scan
         * the body for inline `#tag` occurrences. We skip the frontmatter
         * range during the body scan so YAML literals never get mistaken
         * for inline tags. */
        size_t scan_start = 0;
        size_t fm_start = 0, fm_end = 0, body_start = 0;
        if (frontmatter_scan(data, flen,
                             &fm_start, &fm_end, &body_start))
        {
            fm_each_tag(data + fm_start, fm_end - fm_start, tags_fm_cb, a);
            scan_start = body_start;
        }
        for (size_t i = scan_start; i + 1 < flen; ++i) {
            if (data[i] != '#') continue;
            unsigned char prev = (i == 0) ? 0 : (unsigned char)data[i - 1];
            if (!tag_is_boundary(prev)) continue;
            if (data[i + 1] == '#') continue;
            if (!tag_is_body_byte((unsigned char)data[i + 1])) continue;
            size_t s = i + 1;
            size_t e = s;
            while (e < flen && tag_is_body_byte((unsigned char)data[e])) e++;
            tags_intern(a, data + s, e - s)->count++;
            i = e - 1;
        }
        free(data);
    }
    qsort(a->tags_entries, a->tags_count, sizeof(*a->tags_entries), tags_compare);
}

static void tags_open(App* a)
{
    tags_collect(a);
    a->tags_active   = true;
    a->tags_selected = a->tags_count > 0 ? 0 : -1;
    a->tags_hover    = -1;
    a->tags_scroll   = 0;
}

static void tags_close(App* a) { a->tags_active = false; }

static int tags_hit_test(const App* a, int mx, int my)
{
    int rh    = tags_row_h(a);
    int box_w = TAGS_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = TAGS_BOX_Y;
    int max_box_h = a->win_h - 80;
    int box_h = rh * (a->tags_count + 3) + 24;
    if (box_h > max_box_h) box_h = max_box_h;
    int rows_top = box_y + rh + 12;
    int rows_bot = box_y + box_h - rh - 8;
    if (mx < box_x + 8 || mx >= box_x + box_w - 8) return -1;
    if (my < rows_top || my >= rows_bot) return -1;
    int r = (my - rows_top + a->tags_scroll) / rh;
    if (r < 0 || r >= a->tags_count) return -1;
    return r;
}

static int tags_scrollbar_geom(const App* a,
                               SDL_Rect* track, SDL_Rect* thumb)
{
    int rh    = tags_row_h(a);
    int box_w = TAGS_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = TAGS_BOX_Y;
    int max_box_h = a->win_h - 80;
    int box_h = rh * (a->tags_count + 3) + 24;
    if (box_h > max_box_h) box_h = max_box_h;
    int rows_top = box_y + rh + 12;
    int rows_bot = box_y + box_h - rh - 8;
    return overlay_list_scrollbar_geom(box_x, box_w, rows_top, rows_bot,
                                       rh * a->tags_count,
                                       a->tags_scroll, track, thumb);
}

/* Activate: open vault search overlay pre-loaded with `#tag`. */
static void tags_activate(App* a)
{
    if (a->tags_selected < 0 || a->tags_selected >= a->tags_count) return;
    struct TagEntry* t = &a->tags_entries[a->tags_selected];
    snprintf(a->vsearch_query, sizeof a->vsearch_query, "#%s", t->name);
    a->vsearch_qlen   = strlen(a->vsearch_query);
    a->vsearch_regex  = false;     /* literal so '#' isn't escaped */
    tags_close(a);
    vsearch_open(a);
    vsearch_rebuild(a);
}

static void render_tags(App* a)
{
    if (!a->tags_active) return;

    overlay_backdrop(a);

    int rh    = tags_row_h(a);
    int box_w = TAGS_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = TAGS_BOX_Y;
    int max_box_h = a->win_h - 80;
    int box_h = rh * (a->tags_count + 3) + 24;
    if (box_h > max_box_h) box_h = max_box_h;

    SDL_Rect box = { box_x, box_y, box_w, box_h };
    overlay_card(a, box);

    char title[80];
    snprintf(title, sizeof title, "Tags  (%d)", a->tags_count);
    font_draw_line(a->font_body, title, strlen(title),
                   box_x + 16, box_y + 10 + font_ascent(a->font_body),
                   a->fg_link);

    int rows_top = box_y + rh + 12;
    int rows_bot = box_y + box_h - rh - 8;
    SDL_Rect clip = { box_x + 4, rows_top - 2,
                      box_w - 8, rows_bot - rows_top + 4 };
    SDL_RenderSetClipRect(a->renderer, &clip);

    int y = rows_top - a->tags_scroll;
    for (int i = 0; i < a->tags_count; ++i, y += rh) {
        if (y + rh < rows_top || y > rows_bot) continue;
        struct TagEntry* t = &a->tags_entries[i];
        bool sel = (i == a->tags_selected);
        bool hov = (i == a->tags_hover) && !sel;
        if (sel || hov) {
            SDL_Rect hr = { box_x + 4, y, box_w - 8, rh };
            SDL_Color bc = sel ? a->bg_sidebar_active : a->bg_sidebar_hover;
            SDL_SetRenderDrawColor(a->renderer, bc.r, bc.g, bc.b, 255);
            SDL_RenderFillRect(a->renderer, &hr);
        }
        char tag[80];
        snprintf(tag, sizeof tag, "#%s", t->name);
        font_draw_line(a->font_body, tag, strlen(tag),
                       box_x + 16, row_text_baseline(a->font_body, y, rh),
                       sel ? a->fg : a->fg_link);
        char cnt[16];
        snprintf(cnt, sizeof cnt, "%d", t->count);
        int cw = font_measure(a->font_body, cnt, strlen(cnt));
        font_draw_line(a->font_body, cnt, strlen(cnt),
                       box_x + box_w - 16 - cw,
                       row_text_baseline(a->font_body, y, rh),
                       a->fg_muted);
    }

    if (a->tags_count == 0) {
        const char* empty = "(no #tags found in vault)";
        font_draw_line(a->font_body, empty, strlen(empty),
                       box_x + 16,
                       rows_top + font_ascent(a->font_body),
                       a->fg_muted);
    }
    SDL_RenderSetClipRect(a->renderer, NULL);

    SDL_Rect sb_track, sb_thumb;
    if (tags_scrollbar_geom(a, &sb_track, &sb_thumb))
        overlay_scrollbar_draw(a, &sb_track, &sb_thumb,
                               a->sb_drag == SB_TAGS);

    const char* hint = "\xe2\x86\x91/\xe2\x86\x93 navigate  \xc2\xb7  "
        "Enter \xe2\x86\x92 vault search  \xc2\xb7  Esc close";
    font_draw_line(a->font_body, hint, strlen(hint),
                   box_x + 16,
                   box_y + box_h - 8 - rh + font_ascent(a->font_body),
                   a->fg_muted);
}

/* ----------------------------- template picker -------------------------- */

#define TPL_BOX_W   460
#define TPL_BOX_Y    80

static int tpl_row_h(const App* a) { return font_line_height(a->font_body) + 6; }

/* Walk vault.items and pick everything under `data/templates/`. Cheap: the
 * vault scan already enumerated the directory; we just filter by path. */
static int templates_collect(App* a)
{
    a->tpl_count = 0;
    for (size_t i = 0; i < a->vault.count; ++i) {
        VaultItem* it = &a->vault.items[i];
        if (it->is_dir) continue;
        if (!strstr(it->path, "/templates/") &&
            !strstr(it->path, "\\templates\\"))
            continue;
        size_t nlen = strlen(it->name);
        if (nlen < 4 || strcmp(it->name + nlen - 3, ".md") != 0) continue;
        if (a->tpl_count >= a->tpl_cap) {
            a->tpl_cap = a->tpl_cap ? a->tpl_cap * 2 : 8;
            a->tpl_entries = realloc(a->tpl_entries,
                                     a->tpl_cap * sizeof(*a->tpl_entries));
        }
        struct TemplateEntry* t = &a->tpl_entries[a->tpl_count++];
        snprintf(t->name, sizeof t->name, "%.*s",
                 (int)(nlen - 3), it->name);
        snprintf(t->path, sizeof t->path, "%s", it->path);
    }
    return a->tpl_count;
}

/* Substitute `{{date}}`, `{{time}}`, `{{title}}` in `src[0..src_len)` and
 * write into a freshly malloc'd string. Caller frees. `title` may be NULL
 * (substituted as "untitled"). */
static char* tpl_expand(const char* src, size_t src_len, const char* title)
{
    char date_buf[16], time_buf[16];
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    if (!tm || strftime(date_buf, sizeof date_buf, "%Y-%m-%d", tm) == 0)
        snprintf(date_buf, sizeof date_buf, "today");
    if (!tm || strftime(time_buf, sizeof time_buf, "%H:%M", tm) == 0)
        snprintf(time_buf, sizeof time_buf, "00:00");
    if (!title || !*title) title = "untitled";

    /* Single-pass replace. Output grows on demand. */
    size_t cap = src_len + 64;
    size_t len = 0;
    char*  out = malloc(cap);
    out[0] = 0;

    #define APPEND(s_, n_) do {                                  \
        size_t need = len + (n_) + 1;                           \
        if (need > cap) {                                       \
            while (cap < need) cap *= 2;                        \
            out = realloc(out, cap);                            \
        }                                                       \
        memcpy(out + len, s_, n_);                              \
        len += (n_);                                            \
        out[len] = 0;                                           \
    } while (0)

    size_t i = 0;
    while (i < src_len) {
        if (i + 3 < src_len && src[i] == '{' && src[i+1] == '{') {
            size_t k = i + 2;
            while (k + 1 < src_len &&
                   !(src[k] == '}' && src[k+1] == '}')) k++;
            if (k + 1 < src_len && src[k] == '}' && src[k+1] == '}') {
                size_t name_len = k - (i + 2);
                const char* name = src + i + 2;
                if (name_len == 4 && memcmp(name, "date", 4) == 0) {
                    APPEND(date_buf, strlen(date_buf));
                } else if (name_len == 4 && memcmp(name, "time", 4) == 0) {
                    APPEND(time_buf, strlen(time_buf));
                } else if (name_len == 5 && memcmp(name, "title", 5) == 0) {
                    APPEND(title, strlen(title));
                } else {
                    /* Unknown placeholder: leave it as-is so the user notices. */
                    APPEND(src + i, k + 2 - i);
                }
                i = k + 2;
                continue;
            }
        }
        APPEND(src + i, 1);
        i++;
    }
    #undef APPEND
    return out;
}

static void tpl_open(App* a)
{
    if (templates_collect(a) == 0) {
        /* No templates → fall back to the plain new-file behavior. */
        if (!confirm_discard(a)) return;
        free(a->note_path);
        a->note_path = strdup("(unsaved)");
        buffer_set_text(&a->buf, "", 0);
        reparse_preview(a);
        a->vault.selected = -1;
        a->scroll_y = 0;
        update_window_title(a);
        return;
    }
    a->tpl_active   = true;
    a->tpl_selected = 0;
    a->tpl_hover    = -1;
    a->tpl_scroll   = 0;
}

static void tpl_close(App* a) { a->tpl_active = false; }

static int tpl_hit_test(const App* a, int mx, int my)
{
    int rh    = tpl_row_h(a);
    int box_w = TPL_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = TPL_BOX_Y;
    int max_box_h = a->win_h - 80;
    int box_h = rh * (a->tpl_count + 3) + 24;
    if (box_h > max_box_h) box_h = max_box_h;
    int rows_top = box_y + rh + 12;
    int rows_bot = box_y + box_h - rh - 8;
    if (mx < box_x + 8 || mx >= box_x + box_w - 8) return -1;
    if (my < rows_top || my >= rows_bot) return -1;
    int r = (my - rows_top + a->tpl_scroll) / rh;
    if (r < 0 || r >= a->tpl_count) return -1;
    return r;
}

/* Apply the selected template: load its bytes, expand placeholders, set as
 * the buffer (unsaved). The user names it via Save-As. */
static void tpl_activate(App* a)
{
    if (a->tpl_selected < 0 || a->tpl_selected >= a->tpl_count) {
        tpl_close(a);
        return;
    }
    struct TemplateEntry* t = &a->tpl_entries[a->tpl_selected];
    if (!confirm_discard(a)) { tpl_close(a); return; }

    size_t src_len = 0;
    char*  src = slurp(t->path, &src_len);
    if (!src) {
        char msg[700];
        snprintf(msg, sizeof msg, "template not found: %s", t->path);
        app_notify(a, msg);
        tpl_close(a);
        return;
    }
    char* expanded = tpl_expand(src, src_len, "untitled");
    free(src);

    free(a->note_path);
    a->note_path = strdup("(unsaved)");
    buffer_set_text(&a->buf, expanded, strlen(expanded));
    free(expanded);
    reparse_preview(a);
    a->vault.selected = -1;
    a->scroll_y = 0;
    update_window_title(a);
    tpl_close(a);
}

static void render_template_picker(App* a)
{
    if (!a->tpl_active) return;

    overlay_backdrop(a);

    int rh    = tpl_row_h(a);
    int box_w = TPL_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = TPL_BOX_Y;
    int max_box_h = a->win_h - 80;
    int box_h = rh * (a->tpl_count + 3) + 24;
    if (box_h > max_box_h) box_h = max_box_h;

    SDL_Rect box = { box_x, box_y, box_w, box_h };
    overlay_card(a, box);

    const char* title = "New file from template";
    font_draw_line(a->font_body, title, strlen(title),
                   box_x + 16, box_y + 10 + font_ascent(a->font_body),
                   a->fg_link);

    int rows_top = box_y + rh + 12;
    int rows_bot = box_y + box_h - rh - 8;
    SDL_Rect clip = { box_x + 4, rows_top - 2,
                      box_w - 8, rows_bot - rows_top + 4 };
    SDL_RenderSetClipRect(a->renderer, &clip);

    int y = rows_top - a->tpl_scroll;
    for (int i = 0; i < a->tpl_count; ++i, y += rh) {
        if (y + rh < rows_top || y > rows_bot) continue;
        struct TemplateEntry* t = &a->tpl_entries[i];
        bool sel = (i == a->tpl_selected);
        bool hov = (i == a->tpl_hover) && !sel;
        if (sel || hov) {
            SDL_Rect hr = { box_x + 4, y, box_w - 8, rh };
            SDL_Color bc = sel ? a->bg_sidebar_active : a->bg_sidebar_hover;
            SDL_SetRenderDrawColor(a->renderer, bc.r, bc.g, bc.b, 255);
            SDL_RenderFillRect(a->renderer, &hr);
        }
        font_draw_line(a->font_body, t->name, strlen(t->name),
                       box_x + 16,
                       row_text_baseline(a->font_body, y, rh),
                       sel ? a->fg : a->fg_muted);
    }
    SDL_RenderSetClipRect(a->renderer, NULL);

    const char* hint = "\xe2\x86\x91/\xe2\x86\x93 navigate  \xc2\xb7  "
        "Enter use template  \xc2\xb7  Esc blank";
    font_draw_line(a->font_body, hint, strlen(hint),
                   box_x + 16,
                   row_text_baseline(a->font_body,
                                     box_y + box_h - 8 - rh, rh),
                   a->fg_muted);
}

/* ----------------------------- keybindings overlay ---------------------- */

/* Layout shared by render_keybind and the hit-test. */
#define KBIND_BOX_W   620
#define KBIND_BOX_Y    60

/* Forward decls — defined in the actions section. */
static int         ACTIONS_count(void);
static const char* ACTIONS_name(int i);
static const char* ACTIONS_category(int i);
static const char* default_keystr_for_action(const char* action);
static int         action_is_shadowed(const char* action);
static const char* action_shadower   (const char* action);

/* Find the keystr currently bound to `action`: prefer user override, else
 * the first DEFAULT_KEYS entry mapping any keystr → this action that
 * hasn't been re-bound by the user. Returns empty string if unbound. */
static const char* current_keystr_for(const char* action)
{
    const char* k = user_kbind_keystr_for_action(action);
    if (k) return k;
    return default_keystr_for_action(action);
}

static void keybind_open(App* a)
{
    a->keybind_active    = true;
    a->keybind_selected  = 0;
    a->keybind_hover     = -1;
    a->keybind_capturing = false;
    a->keybind_scroll    = 0;
    a->settings_active   = false;     /* close settings while in keybinds */
}

static void keybind_close(App* a)
{
    a->keybind_active    = false;
    a->keybind_capturing = false;
}

static int kbind_row_h(const App* a) { return font_line_height(a->font_body) + 8; }

/* The keybindings overlay shows two kinds of rows: a category header (one
 * per group) and an action row (one per ACTIONS entry). The action_idx is
 * the index into ACTIONS for action rows, -1 for headers. */
typedef struct {
    int          y;
    int          h;
    int          action_idx;
    const char*  header_text;
} KbindLayoutRow;

#define KBIND_MAX_ROWS 96
static KbindLayoutRow g_kbind_layout[KBIND_MAX_ROWS];
static int            g_kbind_layout_count;
static int            g_kbind_content_h;     /* total y-extent of all rows */

/* (Re)build the row layout from ACTIONS, accumulating y from `rows_top`
 * (which already factors in the scroll offset). Uses ACTIONS_* accessors
 * so we don't depend on the table being visible at this point in the TU. */
static void keybind_layout_build(const App* a, int rows_top)
{
    int rh = kbind_row_h(a);
    int hh = rh + 4;     /* category header: a little taller for breathing */
    int y  = rows_top;
    int n  = 0;
    int an = ACTIONS_count();
    const char* prev_cat = NULL;
    for (int i = 0; i < an; ++i) {
        const char* cat = ACTIONS_category(i);
        if (!cat) cat = "";
        if (!prev_cat || strcmp(prev_cat, cat) != 0) {
            if (n < KBIND_MAX_ROWS) {
                g_kbind_layout[n].y = y;  g_kbind_layout[n].h = hh;
                g_kbind_layout[n].action_idx  = -1;
                g_kbind_layout[n].header_text = cat;
                n++; y += hh;
            }
            prev_cat = cat;
        }
        if (n < KBIND_MAX_ROWS) {
            g_kbind_layout[n].y = y;  g_kbind_layout[n].h = rh;
            g_kbind_layout[n].action_idx  = i;
            g_kbind_layout[n].header_text = NULL;
            n++; y += rh;
        }
    }
    g_kbind_layout_count = n;
    g_kbind_content_h    = y - rows_top;
}

/* Compute the keybindings overlay's scrollbar geometry. Returns 1 if
 * the scrollbar is currently visible (content overflows), 0 otherwise. */
static int keybind_scrollbar_geom(const App* a,
                                  SDL_Rect* track_out, SDL_Rect* thumb_out)
{
    int rh    = kbind_row_h(a);
    int box_w = KBIND_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = KBIND_BOX_Y;
    int max_box_h = a->win_h - 80;
    int rows_top  = box_y + rh + 12;

    keybind_layout_build(a, rows_top - a->keybind_scroll);
    int box_h = rh + 12 + g_kbind_content_h + a->keybind_scroll + rh + 12;
    if (box_h > max_box_h) box_h = max_box_h;
    int rows_bot  = box_y + box_h - rh - 8;
    int visible_h = rows_bot - rows_top;
    if (g_kbind_content_h <= visible_h || visible_h <= 20) return 0;

    int track_w = 10;
    int track_x = box_x + box_w - track_w - 2;
    SDL_Rect track = { track_x, rows_top, track_w, visible_h };
    SDL_Rect inner; sb_inner_track(&track, &inner);
    int thumb_h = inner.h * visible_h / g_kbind_content_h;
    if (thumb_h < 24) thumb_h = 24;
    if (thumb_h > inner.h - 4) thumb_h = inner.h - 4;
    if (thumb_h < 12) thumb_h = inner.h;     /* degenerate; renders flat */
    int max_scroll_v = g_kbind_content_h - visible_h;
    int max_y = inner.h - thumb_h;
    int thumb_y = inner.y +
                  (max_y > 0 && max_scroll_v > 0
                   ? max_y * a->keybind_scroll / max_scroll_v : 0);
    if (track_out) *track_out = track;
    if (thumb_out) *thumb_out = (SDL_Rect){ track_x + 2, thumb_y,
                                            track_w - 4, thumb_h };
    return 1;
}

/* Make sure the row whose action_idx matches keybind_selected lives within
 * the visible band of the scrollable area. Adjusts keybind_scroll if not. */
static void keybind_ensure_selected_visible(App* a)
{
    int rh    = kbind_row_h(a);
    int box_y = KBIND_BOX_Y;
    int max_box_h = a->win_h - 80;
    int rows_top  = box_y + rh + 12;

    /* Build with scroll applied so y values are screen coords. */
    keybind_layout_build(a, rows_top - a->keybind_scroll);
    int box_h = rh + 12 + g_kbind_content_h + a->keybind_scroll + rh + 12;
    if (box_h > max_box_h) box_h = max_box_h;
    int rows_bot = box_y + box_h - rh - 8;

    int sel_y = -1;
    int sel_h = rh;
    for (int i = 0; i < g_kbind_layout_count; ++i) {
        if (g_kbind_layout[i].action_idx == a->keybind_selected) {
            sel_y = g_kbind_layout[i].y;
            sel_h = g_kbind_layout[i].h;
            break;
        }
    }
    if (sel_y < 0) return;
    if (sel_y < rows_top)            a->keybind_scroll += rows_top - sel_y;
    if (sel_y + sel_h > rows_bot)    a->keybind_scroll += (sel_y + sel_h) - rows_bot;
    if (a->keybind_scroll < 0) a->keybind_scroll = 0;
}

/* Hit-test: returns ACTIONS index under (mx, my), or -1 (header / outside). */
static int keybind_hit_test(const App* a, int mx, int my)
{
    int rh    = kbind_row_h(a);
    int box_w = KBIND_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = KBIND_BOX_Y;
    int max_box_h = a->win_h - 80;

    /* Build layout against rows_top with scroll already applied so we can
     * compare y-coordinates directly. */
    int rows_top = box_y + rh + 12 - a->keybind_scroll;
    keybind_layout_build(a, rows_top);

    int box_h = rh + 12 + g_kbind_content_h + a->keybind_scroll + rh + 12;
    if (box_h > max_box_h) box_h = max_box_h;
    int visible_top = box_y + rh + 12;
    int visible_bot = box_y + box_h - rh - 8;

    if (mx < box_x + 8 || mx >= box_x + box_w - 8) return -1;
    if (my < visible_top || my >= visible_bot) return -1;

    for (int i = 0; i < g_kbind_layout_count; ++i) {
        KbindLayoutRow* r = &g_kbind_layout[i];
        if (my >= r->y && my < r->y + r->h) {
            return r->action_idx;     /* -1 for header rows */
        }
    }
    return -1;
}

static void render_keybind(App* a)
{
    if (!a->keybind_active) return;

    /* Backdrop. */
    overlay_backdrop(a);

    int rh    = kbind_row_h(a);
    int box_w = KBIND_BOX_W;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = KBIND_BOX_Y;
    int max_box_h = a->win_h - 80;

    /* Build the layout first so we know how tall the box should be. */
    int rows_top = box_y + rh + 12 - a->keybind_scroll;
    keybind_layout_build(a, rows_top);

    /* Box height: title + (max content) + hint. Cap to window. */
    int desired_h = rh + 12 + g_kbind_content_h + a->keybind_scroll
                    + rh + 12;
    int box_h = desired_h < max_box_h ? desired_h : max_box_h;

    SDL_Rect box = { box_x, box_y, box_w, box_h };
    overlay_card(a, box);

    /* Title */
    {
        const char* title = a->keybind_capturing
            ? "Keybindings \xe2\x80\x94 press a key combo (Esc to cancel)"
            : "Keybindings";
        font_draw_line(a->font_body, title, strlen(title),
                       box_x + 16, box_y + 10 + font_ascent(a->font_body),
                       a->fg_link);
    }

    /* Clip rows to the box interior. */
    int visible_top = box_y + rh + 12;
    int visible_bot = box_y + box_h - rh - 8;
    SDL_Rect clip = { box_x + 4, visible_top - 4,
                      box_w - 8, visible_bot - visible_top + 4 };
    SDL_RenderSetClipRect(a->renderer, &clip);

    /* Warning tint for shadowed actions. */
    SDL_Color warn_c = { 230, 170, 60, 255 };

    for (int i = 0; i < g_kbind_layout_count; ++i) {
        KbindLayoutRow* row = &g_kbind_layout[i];
        if (row->y + row->h < visible_top || row->y > visible_bot) continue;

        if (row->action_idx < 0) {
            /* Category header — small caps style label + thin underline. */
            font_draw_line(a->font_body,
                row->header_text, strlen(row->header_text),
                box_x + 16, row->y + font_ascent(a->font_body) + 4,
                a->fg_quote);
            SDL_Rect underline = { box_x + 16, row->y + row->h - 2,
                                   box_w - 32, 1 };
            SDL_SetRenderDrawColor(a->renderer,
                a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 90);
            SDL_RenderFillRect(a->renderer, &underline);
            continue;
        }

        bool sel = (row->action_idx == a->keybind_selected);
        bool hov = (row->action_idx == a->keybind_hover) && !sel;
        if (sel || hov) {
            SDL_Rect hr = { box_x + 4, row->y, box_w - 8, row->h };
            SDL_Color bc = sel ? a->bg_sidebar_active : a->bg_sidebar_hover;
            SDL_SetRenderDrawColor(a->renderer, bc.r, bc.g, bc.b, 255);
            SDL_RenderFillRect(a->renderer, &hr);
        }
        const char* act = ACTIONS_name(row->action_idx);
        const char* ks  = current_keystr_for(act);
        bool unbound   = (*ks == 0);
        bool shadowed  = unbound && action_is_shadowed(act);

        SDL_Color label_c = sel ? a->fg : a->fg_muted;
        font_draw_line(a->font_body, act, strlen(act),
                       box_x + 28, row->y + font_ascent(a->font_body) + 2,
                       label_c);

        /* Warning glyph in front of the label for shadowed rows. */
        if (shadowed) {
            font_draw_line(a->font_body, "\xe2\x9a\xa0", 3,    /* ⚠ */
                           box_x + 12,
                           row->y + font_ascent(a->font_body) + 2,
                           warn_c);
        }

        const char* show;
        char shadowed_buf[80];
        SDL_Color val_c;
        if (a->keybind_capturing && sel) {
            show  = "press a key\xe2\x80\xa6";
            val_c = a->fg_link;
        } else if (shadowed) {
            const char* who = action_shadower(act);
            if (who) snprintf(shadowed_buf, sizeof shadowed_buf,
                              "(shadowed by %s)", who);
            else     snprintf(shadowed_buf, sizeof shadowed_buf,
                              "(shadowed)");
            show  = shadowed_buf;
            val_c = warn_c;
        } else if (unbound) {
            show  = "(unbound)";
            val_c = a->fg_muted;
        } else {
            show  = ks;
            val_c = a->fg_link;
        }
        font_draw_line(a->font_body, show, strlen(show),
                       box_x + 280, row->y + font_ascent(a->font_body) + 2,
                       val_c);
    }
    SDL_RenderSetClipRect(a->renderer, NULL);

    /* Scrollbar — interactive: 10px-wide track on the right edge,
     * draggable thumb, ▲/▼ step buttons. Geom is shared with the event
     * handlers via keybind_scrollbar_geom. */
    {
        SDL_Rect track, thumb;
        if (keybind_scrollbar_geom(a, &track, &thumb))
            overlay_scrollbar_draw(a, &track, &thumb,
                                   a->sb_drag == SB_KEYBIND);
    }

    /* Hint at the bottom. */
    const char* hint = a->keybind_capturing
        ? "Press a key combo to bind, or Esc to cancel"
        : "Enter / Click to capture  -  Del to clear  -  Esc to close";
    font_draw_line(a->font_body, hint, strlen(hint),
                   box_x + 16,
                   box_y + box_h - 8 - rh + font_ascent(a->font_body),
                   a->fg_muted);
}

/* Whitespace-separated word count of `data[0..len)`. Cheap O(N); recomputed
 * each render frame because notes are typically <100 KB. */
static int buf_word_count(const char* data, size_t len)
{
    int  words = 0;
    bool in_word = false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)data[i];
        bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                   c == '\f' || c == '\v');
        if (!ws && !in_word) { words++; in_word = true; }
        else if (ws)         { in_word = false; }
    }
    return words;
}

/* Format `words` and a 200-wpm reading-time estimate into `out`. */
static void word_count_str(int words, char* out, size_t cap)
{
    int mins = (words + 199) / 200;
    if (mins <= 1) snprintf(out, cap, "%d words", words);
    else           snprintf(out, cap, "%d words \xc2\xb7 %d min read", words, mins);
}

/* Live-resize indicator. Two pieces:
 *  (1) a faint border outline around the entire client area, so the user
 *      can see the live new bounds while dragging an edge.
 *  (2) a centered "WxH" pill that fades out shortly after the user stops.
 *
 * On Windows, SDL's main loop is blocked during the modal resize drag, so
 * SIZE_CHANGED events would only fire on mouse-up if we waited for the
 * main loop. resize_event_watch (installed in app_init) catches the event
 * synchronously and forces an app_render, which is what actually lets
 * this badge appear during the drag. */
static void render_resize_badge(App* a)
{
    uint32_t now = SDL_GetTicks();
    if (now >= a->resize_show_until) return;

    /* Fade out over the last ~250ms. */
    int remain = (int)(a->resize_show_until - now);
    float alpha_f = remain > 250 ? 1.0f : (remain / 250.0f);
    Uint8 alpha   = (Uint8)(alpha_f * 255.0f);

    /* (1) Outline of the new window bounds — drawn as four thin filled
     * strips so it stays sharp at any DPI without needing AA. */
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_link.r, a->fg_link.g, a->fg_link.b,
        (Uint8)(alpha * 0.55f));
    int t = 2;
    SDL_Rect top    = { 0, 0, a->win_w, t };
    SDL_Rect bot    = { 0, a->win_h - t, a->win_w, t };
    SDL_Rect left   = { 0, 0, t, a->win_h };
    SDL_Rect right_ = { a->win_w - t, 0, t, a->win_h };
    SDL_RenderFillRect(a->renderer, &top);
    SDL_RenderFillRect(a->renderer, &bot);
    SDL_RenderFillRect(a->renderer, &left);
    SDL_RenderFillRect(a->renderer, &right_);

    /* (2) WxH badge in the centre. */
    char label[64];
    snprintf(label, sizeof label, "%d x %d", a->win_w, a->win_h);

    Font* f = a->font_h2 ? a->font_h2 : a->font_body;
    int sz_y = font_line_height(f);
    int lw   = font_measure(f, label, strlen(label));
    int pad_x = 24, pad_y = 12;
    int w = lw + pad_x * 2;
    int h = sz_y + pad_y * 2;
    int x = (a->win_w - w) / 2;
    int y = (a->win_h - h) / 2;

    SDL_SetRenderDrawColor(a->renderer, 20, 22, 28,
                           (Uint8)(alpha * 0.9f));
    fill_rrect(a->renderer, (SDL_Rect){x, y, w, h}, 14);
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_link.r, a->fg_link.g, a->fg_link.b,
        (Uint8)(alpha * 0.6f));
    draw_rrect(a->renderer, (SDL_Rect){x, y, w, h}, 14);

    SDL_Color tc = a->fg;
    tc.a = alpha;
    font_draw_line(f, label, strlen(label),
                   x + pad_x,
                   y + pad_y + font_ascent(f),
                   tc);

    /* Keep the loop awake so the fade actually animates. */
    a->wants_anim_frame = true;
}

/* Floating tooltip that follows the cursor when over a link in preview.
 * The text is filled in by the MOUSEMOTION handler — this just renders
 * the pill. Color signals state: muted for resolved links, red-ish for
 * broken wiki targets so the user sees the dead-end before clicking. */
static void render_link_tooltip(App* a)
{
    if (!a->tip_active || !a->tip_text[0]) return;
    Font* f = a->font_body;
    int sz_y  = font_line_height(f);
    int pad_x = 10;
    int pad_y = 6;
    int tw    = font_measure(f, a->tip_text, strlen(a->tip_text));
    int max_w = a->win_w - 24;
    /* Truncate with leading "..." so the URL / path tail remains visible. */
    char shown[256];
    if (tw > max_w) {
        const char* ell = "...";
        int ell_w = font_measure(f, ell, strlen(ell));
        int budget = max_w - ell_w - 2 * pad_x;
        if (budget < 40) budget = 40;
        size_t n = strlen(a->tip_text);
        size_t keep = 0;
        int w = 0;
        for (size_t i = n; i > 0; ) {
            size_t j = i - 1;
            while (j > 0 && ((unsigned char)a->tip_text[j] & 0xC0) == 0x80) j--;
            int cw = font_measure(f, a->tip_text + j, i - j);
            if (w + cw > budget) break;
            w += cw;
            keep = n - j;
            i = j;
        }
        snprintf(shown, sizeof shown, "%s%s", ell,
                 a->tip_text + (n - keep));
        tw = font_measure(f, shown, strlen(shown));
    } else {
        snprintf(shown, sizeof shown, "%s", a->tip_text);
    }

    int w = tw + 2 * pad_x;
    int h = sz_y + 2 * pad_y;
    /* Anchor just below-right of the cursor; clamp into the window. */
    int x = a->tip_anchor_x + 16;
    int y = a->tip_anchor_y + 20;
    if (x + w > a->win_w - 6) x = a->win_w - 6 - w;
    if (y + h > a->win_h - 6) y = a->tip_anchor_y - h - 8;
    if (x < 6) x = 6;
    if (y < 6) y = 6;

    /* Soft drop shadow + body fill. */
    SDL_SetRenderDrawColor(a->renderer, 0, 0, 0, 110);
    fill_rrect(a->renderer, (SDL_Rect){x + 2, y + 3, w, h}, h / 2);
    SDL_Color body = a->bg_sidebar_active;
    body.a = 240;
    SDL_SetRenderDrawColor(a->renderer, body.r, body.g, body.b, body.a);
    fill_rrect(a->renderer, (SDL_Rect){x, y, w, h}, h / 2);

    /* Subtle accent border that matches link / broken-link semantics. */
    SDL_Color border = a->tip_broken
        ? (SDL_Color){230, 110, 110, 200}
        : (SDL_Color){a->fg_link.r, a->fg_link.g, a->fg_link.b, 200};
    SDL_SetRenderDrawColor(a->renderer,
        border.r, border.g, border.b, border.a);
    draw_rrect(a->renderer, (SDL_Rect){x, y, w, h}, h / 2);

    SDL_Color tc = a->tip_broken
        ? (SDL_Color){240, 180, 180, 255}
        : a->fg;
    font_draw_line(f, shown, strlen(shown),
                   x + pad_x, y + pad_y + font_ascent(f), tc);
}

/* Event watch: fires synchronously when SDL pushes a window event. On
 * Windows this is the ONLY way to react during the modal resize drag —
 * the main loop is stuck inside DefWindowProc, but the WndProc still
 * calls into SDL which pushes events, and our watch sees them. */
static int SDLCALL resize_event_watch(void* userdata, SDL_Event* e)
{
    App* a = userdata;
    if (e->type == SDL_WINDOWEVENT &&
        e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED &&
        a->window && a->renderer &&
        e->window.windowID == SDL_GetWindowID(a->window))
    {
        a->win_w = e->window.data1;
        a->win_h = e->window.data2;
        a->resize_show_until = SDL_GetTicks() + 900;
        clamp_scroll(a);
        app_render(a);
    }
    return 1;     /* keep event in queue for the main loop too */
}

static void render_status(App* a)
{
    int sh = status_bar_h(a);
    int sy = a->win_h - sh;
    int by = sy + font_ascent(a->font_body) + 4;

    SDL_Rect bg = { 0, sy, a->win_w, sh };
    SDL_SetRenderDrawColor(a->renderer,
        a->bg_status.r, a->bg_status.g, a->bg_status.b, 255);
    SDL_RenderFillRect(a->renderer, &bg);

    /* Hairline divider above status bar. */
    SDL_Rect div = { 0, sy - 1, a->win_w, 1 };
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 70);
    SDL_RenderFillRect(a->renderer, &div);

    uint32_t now = SDL_GetTicks();
    if (a->notification_msg && now < a->notification_until) {
        font_draw_line(a->font_body, a->notification_msg,
                       strlen(a->notification_msg),
                       12, by, a->fg_link);
        return;
    }

    /* Build the left-side status string and draw it as plain muted text —
     * no badges, no chips. The accent on the mode prefix and word count is
     * what carries the visual weight. The Ln/Col indicator lives on the
     * right (rendered below) so it's visible in both modes. */
    char buf[512];
    if (a->edit_mode && buffer_has_selection(&a->buf)) {
        size_t lo, hi;
        buffer_get_selection(&a->buf, &lo, &hi);
        snprintf(buf, sizeof buf, "%s%s    %zu sel",
                 vault_basename(a->note_path),
                 a->buf.dirty ? " *" : "", hi - lo);
    } else if (!a->edit_mode &&
               a->preview_sel_start >= 0 &&
               a->preview_sel_end != (size_t)a->preview_sel_start)
    {
        size_t lo = (size_t)a->preview_sel_start, hi = a->preview_sel_end;
        if (lo > hi) { size_t t = lo; lo = hi; hi = t; }
        snprintf(buf, sizeof buf, "%s%s    %zu sel  (Ctrl+C to copy)",
                 vault_basename(a->note_path),
                 a->buf.dirty ? " *" : "", hi - lo);
    } else if (a->edit_mode) {
        snprintf(buf, sizeof buf, "%s%s",
                 vault_basename(a->note_path),
                 a->buf.dirty ? " *" : "");
    } else {
        snprintf(buf, sizeof buf, "%s%s    Ctrl+E to edit",
                 a->note_path ? vault_basename(a->note_path) : "(unsaved)",
                 a->buf.dirty ? " *" : "");
    }

    /* Mode prefix in accent color, then the rest in muted. */
    const char* mode = a->edit_mode ? "EDIT" : "PREVIEW";
    int x = 12;
    font_draw_line(a->font_body, mode, strlen(mode), x, by, a->fg_link);
    x += font_measure(a->font_body, mode, strlen(mode)) + 12;
    font_draw_line(a->font_body, buf, strlen(buf), x, by, a->fg_status);

    /* Right-aligned cluster: word count, then Ln/Col position. Position is
     * always shown (the buffer cursor is meaningful even in preview); the
     * total-line count provides a sense of where you are in the doc. */
    SDL_Color accent_muted = {
        (Uint8)(a->fg_muted.r + (a->fg_link.r - a->fg_muted.r) / 4),
        (Uint8)(a->fg_muted.g + (a->fg_link.g - a->fg_muted.g) / 4),
        (Uint8)(a->fg_muted.b + (a->fg_link.b - a->fg_muted.b) / 4),
        255
    };
    int rx = a->win_w - 12;

    /* Position indicator: Ln X / N · Col Y */
    {
        size_t line, col;
        buffer_cursor_pos(&a->buf, &line, &col);
        size_t total = buffer_line_count(&a->buf);
        char pos[80];
        snprintf(pos, sizeof pos, "Ln %zu / %zu  Col %zu",
                 line + 1, total, col + 1);
        int pw = font_measure(a->font_body, pos, strlen(pos));
        rx -= pw;
        font_draw_line(a->font_body, pos, strlen(pos), rx, by, accent_muted);
        rx -= 18;     /* gap before next item */
    }

    /* Word count (further left of the position chip). */
    const char* wd = a->buf.data;
    size_t      wl = a->buf.len;
    if (a->fm_present && a->fm_body_start <= a->buf.len) {
        wd += a->fm_body_start;
        wl -= a->fm_body_start;
    }
    char wc[64];
    word_count_str(buf_word_count(wd, wl), wc, sizeof wc);
    int wc_w = font_measure(a->font_body, wc, strlen(wc));
    rx -= wc_w;
    font_draw_line(a->font_body, wc, strlen(wc), rx, by, accent_muted);
}

static void render_search_overlay(App* a);
static void render_switcher      (App* a);
static void render_cmdp          (App* a);
static void render_plugins       (App* a);
static void render_resize_badge  (App* a);
static void render_link_tooltip  (App* a);
static void render_wiki_complete (App* a);
static void render_context_menu  (App* a);
static void render_recent_submenu(App* a);
static void render_settings      (App* a);
static void render_help          (App* a);
static void render_keybind       (App* a);
static void render_picker        (App* a);
static void render_vsearch       (App* a);
static void render_outline       (App* a);
static void render_outline_panel (App* a);
static void render_backlinks     (App* a);
static void render_tags          (App* a);
static void render_template_picker(App* a);
static void render_find_highlights(App* a);
static void keybind_open         (App* a);
static void keybind_close        (App* a);

/* Lerp `cur` toward `target` by an exponential rate (per-second). dt is the
 * frame time in seconds. Returns the new value. With rate ≈ 14 the half-life
 * is ~50ms which feels snappy without being snap. */
static float anim_step(float cur, float target, float dt, float rate)
{
    float k = 1.0f - expf(-rate * dt);
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    return cur + (target - cur) * k;
}

/* Cubic ease-out: smooth out the tail end of a 0→1 progress value so motion
 * feels like it settles rather than just stopping. */
static float ease_out_cubic(float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

/* Per-frame UI animation tick. Eases all chrome buttons toward their target
 * hover state, decays press flashes, advances ctx-menu fade-in. Sets
 * wants_anim_frame so the main loop polls at 60fps while anything is moving. */
static void app_animate(App* a)
{
    uint32_t now = SDL_GetTicks();
    float    dt  = (now - a->anim_last_ms) / 1000.0f;
    if (a->anim_last_ms == 0) dt = 0.016f;
    if (dt > 0.10f) dt = 0.10f;     /* clamp huge gaps (background tab) */
    a->anim_last_ms = now;

    bool moving = false;
    /* Chrome bar buttons: ease toward 1 if hovered, 0 if not. */
    for (int i = 0; i < CB_COUNT; ++i) {
        float target = (a->chrome_hover == i) ? 1.0f : 0.0f;
        float prev   = a->chrome_hover_t[i];
        a->chrome_hover_t[i] = anim_step(prev, target, dt, 18.0f);
        if (fabsf(a->chrome_hover_t[i] - target) > 0.005f) moving = true;
        a->chrome_press_t[i] = anim_step(a->chrome_press_t[i], 0.0f, dt, 9.0f);
        if (a->chrome_press_t[i] > 0.005f) moving = true;
    }

    /* Breadcrumb segment hover. */
    for (int i = 0; i < 2; ++i) {
        float target = (a->crumb_hover == i) ? 1.0f : 0.0f;
        a->crumb_hover_t[i] = anim_step(a->crumb_hover_t[i], target, dt, 18.0f);
        if (fabsf(a->crumb_hover_t[i] - target) > 0.005f) moving = true;
    }

    /* Title bar window-control buttons. */
    for (int i = 0; i < 3; ++i) {
        float target = (a->tb_btn_hover == i) ? 1.0f : 0.0f;
        a->tb_btn_hover_t[i] = anim_step(a->tb_btn_hover_t[i], target,
                                         dt, 18.0f);
        if (fabsf(a->tb_btn_hover_t[i] - target) > 0.005f) moving = true;
    }
    /* Menu bar items. */
    for (int i = 0; i < 4; ++i) {
        float target = (a->menu_hover == i) ? 1.0f : 0.0f;
        a->menu_hover_t[i] = anim_step(a->menu_hover_t[i], target, dt, 18.0f);
        if (fabsf(a->menu_hover_t[i] - target) > 0.005f) moving = true;
    }

    /* Context menu open animation. */
    {
        float target = a->ctx_menu_active ? 1.0f : 0.0f;
        a->ctx_menu_open_t = anim_step(a->ctx_menu_open_t, target, dt, 22.0f);
        if (fabsf(a->ctx_menu_open_t - target) > 0.005f) moving = true;
        for (int r = 0; r < 16; ++r) {
            float t = (a->ctx_menu_active && r == a->ctx_menu_hover) ? 1.0f : 0.0f;
            a->ctx_menu_row_t[r] = anim_step(a->ctx_menu_row_t[r], t, dt, 18.0f);
            if (fabsf(a->ctx_menu_row_t[r] - t) > 0.005f) moving = true;
        }
    }
    /* Recent-vaults submenu animation — same shape as the parent. */
    {
        float target = a->ctx_submenu_active ? 1.0f : 0.0f;
        a->ctx_submenu_open_t =
            anim_step(a->ctx_submenu_open_t, target, dt, 22.0f);
        if (fabsf(a->ctx_submenu_open_t - target) > 0.005f) moving = true;
        for (int r = 0; r < 16; ++r) {
            float t = (a->ctx_submenu_active && r == a->ctx_submenu_hover)
                ? 1.0f : 0.0f;
            a->ctx_submenu_row_t[r] =
                anim_step(a->ctx_submenu_row_t[r], t, dt, 18.0f);
            if (fabsf(a->ctx_submenu_row_t[r] - t) > 0.005f) moving = true;
        }
    }

    a->wants_anim_frame = moving;
}

static void app_render(App* a)
{
    app_animate(a);

    SDL_SetRenderDrawColor(a->renderer, a->bg.r, a->bg.g, a->bg.b, a->bg.a);
    SDL_RenderClear(a->renderer);

    a->hit_count = 0;        /* refilled by styled_run during preview */
    a->preview_row_count = 0;

    if (a->edit_mode) a->doc_height_px = render_editor(a);
    else              a->doc_height_px = render_preview(a, true);

    if (!a->edit_mode) render_preview_selection(a);
    if (a->edit_mode) render_find_highlights(a);
    render_scrollbar(a);
    render_sidebar(a);
    render_outline_panel(a);
    render_chrome(a);
    render_search_overlay(a);
    render_switcher(a);
    render_cmdp(a);
    render_plugins(a);
    render_wiki_complete(a);
    render_context_menu(a);
    render_recent_submenu(a);
    render_settings(a);
    render_keybind(a);
    render_picker(a);
    render_vsearch(a);
    render_outline(a);
    render_backlinks(a);
    render_tags(a);
    render_template_picker(a);
    render_help(a);
    render_dnd_ghost(a);
    render_status(a);
    /* Resize badge / link tooltip / confirm + text-input modals render
     * LAST so they sit on top of everything else, including overlays
     * that may still be visible. */
    render_resize_badge(a);
    render_link_tooltip(a);
    /* tinput modal first, then rename popup, confirm modal LAST. Order
     * matters when one is nested inside the other (e.g. Delete-confirm
     * or Rename popup fired from inside the folder picker) — whichever
     * renders later wins the click. */
    render_tinput_modal(a);
    render_rename_popup(a);
    render_confirm_modal(a);

    SDL_RenderPresent(a->renderer);
}

/* Highlight all current find matches in edit mode (orange-ish tint behind
 * matched bytes). Walks lines and intersects each with each match. */
static void render_find_highlights(App* a)
{
    if (a->search_mode == 0 || a->search_count == 0) return;
    Buffer* b = &a->buf;
    int xL = doc_x_left(a);
    int y  = doc_y_top(a) - a->scroll_y;

    SDL_Rect clip = { xL, chrome_bar_h(a), doc_x_right(a) - xL,
                      a->win_h - status_bar_h(a) - chrome_bar_h(a) };
    SDL_RenderSetClipRect(a->renderer, &clip);

    size_t n_lines = buffer_line_count(b);
    bool   in_fence = false;
    for (size_t li = 0; li < n_lines; ++li) {
        size_t ls   = buffer_line_start(b, li);
        size_t le   = buffer_line_end(b, li);
        size_t llen = le - ls;
        Font*  lf   = edit_line_font(a, b->data + ls, llen);
        int    lh   = line_step(a, lf);
        bool   line_is_fence = is_fence_line(b->data + ls, llen);
        bool   draw_in_fence = in_fence || line_is_fence;
        bool   visible       = (y + lh > 0) && (y < a->win_h);

        if (visible) {
            for (size_t m = 0; m < a->search_count; ++m) {
                size_t ms   = a->search_matches[m];
                size_t mlen = a->search_match_lens
                              ? a->search_match_lens[m]
                              : a->search_qlen;
                size_t me   = ms + mlen;
                if (me <= ls || ms >= le) continue;
                size_t s = ms > ls ? ms : ls;
                size_t e = me < le ? me : le;
                /* Use edit_line_x_at so proportional body-font lines compute
                 * the correct match rectangle x-extent. */
                int sx = xL + MARGIN_X +
                         edit_line_x_at(a, b->data + ls, llen,
                                        s > ls ? s - ls : 0, lf, draw_in_fence);
                int ex = xL + MARGIN_X +
                         edit_line_x_at(a, b->data + ls, llen,
                                        e > ls ? e - ls : 0, lf, draw_in_fence);
                bool is_current = ((int)m == a->search_current);
                SDL_Rect r = { sx, y, ex - sx, lh };
                if (is_current) SDL_SetRenderDrawColor(a->renderer, 220, 160,  60, 140);
                else            SDL_SetRenderDrawColor(a->renderer, 180, 130,  40,  90);
                SDL_RenderFillRect(a->renderer, &r);
            }
        }
        if (line_is_fence) in_fence = !in_fence;
        y += lh;
    }
    SDL_RenderSetClipRect(a->renderer, NULL);
}

/* ----------------------------- mode + actions --------------------------- */

static void enter_edit_mode(App* a)
{
    if (a->edit_mode) return;
    a->edit_mode = true;
    a->scroll_y  = 0;
    SDL_StartTextInput();
    /* Match positions are tied to whichever buffer is shown; re-scan so
     * highlights line up with the new view. */
    if (a->search_mode != 0) search_rebuild(a);
}

static void enter_preview_mode(App* a)
{
    if (!a->edit_mode) return;
    a->edit_mode = false;
    a->scroll_y  = 0;
    SDL_StopTextInput();
    reparse_preview(a);
    if (a->search_mode != 0) search_rebuild(a);
}

static void ensure_cursor_visible(App* a)
{
    if (!a->edit_mode) return;
    size_t line, col;
    buffer_cursor_pos(&a->buf, &line, &col);
    /* Walk lines accumulating heights — necessary because edit mode now
     * has variable per-line height (heading lines are taller). */
    int cy = 0;
    for (size_t i = 0; i < line; ++i) {
        size_t ls = buffer_line_start(&a->buf, i);
        size_t le = buffer_line_end(&a->buf, i);
        Font* lf = edit_line_font(a, a->buf.data + ls, le - ls);
        cy += line_step(a, lf);
    }
    size_t ls = buffer_line_start(&a->buf, line);
    size_t le = buffer_line_end(&a->buf, line);
    Font* lf = edit_line_font(a, a->buf.data + ls, le - ls);
    int lh = line_step(a, lf);
    int vh = viewport_h(a);
    if (cy < a->scroll_y)            a->scroll_y = cy;
    if (cy + lh > a->scroll_y + vh)  a->scroll_y = cy + lh - vh;
    if (a->scroll_y < 0)             a->scroll_y = 0;
}

static void open_file_picker(App* a)
{
    if (!confirm_discard(a)) return;
    char* picked = vault_open_dialog(a->window);
    if (!picked) return;
    load_note(a, picked);
    if (a->vault.selected < 0) {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", picked);
        char* slash = strrchr(dir, '/');
        char* bs    = strrchr(dir, '\\');
        char* last  = slash > bs ? slash : bs;
        if (last) *last = 0;
        if (dir[0]) {
            vault_scan(&a->vault, dir);
            a->vault.selected = vault_index_of(&a->vault, picked);
        }
    }
    free(picked);
}

/* Detect majority line ending in the buffer (LF count vs CRLF count).
 * Returns 1 (LF) or 2 (CRLF). Treats files with neither as LF. */
static int buf_detect_line_endings(const Buffer* b)
{
    size_t crlf = 0, lf = 0;
    for (size_t i = 0; i < b->len; ++i) {
        if (b->data[i] == '\n') {
            if (i > 0 && b->data[i - 1] == '\r') crlf++;
            else lf++;
        }
    }
    return crlf > lf ? 2 : 1;
}

/* Save the buffer respecting the line-ending preference. cfg_line_endings:
 *   0 = preserve (auto-detect majority and rewrite consistently)
 *   1 = always LF, 2 = always CRLF
 * Writes via a temp pass that strips '\r' chars when emitting LF, and
 * inserts '\r' before any bare '\n' when emitting CRLF. */
static int save_note_with_eol(App* a, const char* path)
{
    int policy = a->cfg_line_endings;
    if (policy == 0) policy = buf_detect_line_endings(&a->buf);

    FILE* f = fopen(path, "wb");
    if (!f) return -1;

    const char* d = a->buf.data;
    size_t n = a->buf.len;
    if (policy == 1) {
        /* LF: drop every \r. */
        for (size_t i = 0; i < n; ++i) {
            if (d[i] == '\r') continue;
            fputc(d[i], f);
        }
    } else {
        /* CRLF: ensure every \n is preceded by \r. */
        for (size_t i = 0; i < n; ++i) {
            if (d[i] == '\n' && (i == 0 || d[i - 1] != '\r')) {
                fputc('\r', f);
            }
            fputc(d[i], f);
        }
    }
    fclose(f);
    a->buf.saved_head = a->buf.op_head;
    a->buf.dirty = false;
    return 0;
}

static void save_note(App* a)
{
    if (!a->note_path) return;
    if (save_note_with_eol(a, a->note_path) == 0) {
        update_window_title(a);
        fprintf(stderr, "saved %s\n", a->note_path);
    } else {
        fprintf(stderr, "save failed: %s\n", a->note_path);
    }
}

static void edit_copy(App* a)
{
    if (!buffer_has_selection(&a->buf)) return;
    size_t lo, hi;
    buffer_get_selection(&a->buf, &lo, &hi);
    char* tmp = malloc(hi - lo + 1);
    memcpy(tmp, a->buf.data + lo, hi - lo);
    tmp[hi - lo] = 0;
    SDL_SetClipboardText(tmp);
    free(tmp);
}

static void preview_copy(App* a)
{
    if (a->preview_sel_start < 0) return;
    size_t lo = (size_t)a->preview_sel_start, hi = a->preview_sel_end;
    if (lo > hi) { size_t t = lo; lo = hi; hi = t; }
    if (lo == hi) return;
    if (lo >= a->doc.len) return;
    if (hi >  a->doc.len) hi = a->doc.len;
    char* tmp = malloc(hi - lo + 1);
    memcpy(tmp, a->doc.data + lo, hi - lo);
    tmp[hi - lo] = 0;
    SDL_SetClipboardText(tmp);
    free(tmp);
}

static void edit_cut(App* a)
{
    edit_copy(a);
    buffer_delete_selection(&a->buf);
}

static void edit_paste(App* a)
{
    char* t = SDL_GetClipboardText();
    if (!t) return;
    buffer_undo_break(&a->buf);
    if (*t) buffer_insert(&a->buf, t, strlen(t));
    buffer_undo_break(&a->buf);
    SDL_free(t);
}

static size_t edit_position_at(const App* a, int mx, int my)
{
    int xL = doc_x_left(a);
    int local_y = my - doc_y_top(a) + a->scroll_y;
    if (local_y < 0) return 0;

    /* Find the target line by accumulating per-line heights (variable).
     * Track fence state along the way so the byte-walk below uses the
     * correct styling for the target line. */
    size_t n_lines    = buffer_line_count(&a->buf);
    size_t target_line = 0;
    int    y_acc      = 0;
    bool   in_fence   = false;
    Font*  lf         = a->font_code;
    for (size_t i = 0; i < n_lines; ++i) {
        size_t ls = buffer_line_start(&a->buf, i);
        size_t le = buffer_line_end(&a->buf, i);
        lf = edit_line_font(a, a->buf.data + ls, le - ls);
        int lh = line_step(a, lf);
        if (y_acc + lh > local_y) { target_line = i; break; }
        if (is_fence_line(a->buf.data + ls, le - ls)) in_fence = !in_fence;
        y_acc += lh;
        target_line = i;
    }

    size_t ls = buffer_line_start(&a->buf, target_line);
    size_t le = buffer_line_end  (&a->buf, target_line);
    /* Recompute font for the target line (in case the loop ran past last). */
    lf = edit_line_font(a, a->buf.data + ls, le - ls);
    bool target_in_fence =
        in_fence || is_fence_line(a->buf.data + ls, le - ls);
    int local_x = mx - xL - MARGIN_X;
    if (local_x <= 0) return ls;

    /* Walk per-style runs from line start, accumulating x. Body lines and
     * fence lines both use the per-style path; only heading lines fall
     * through to the single-font walk below since their family has no
     * bold/italic variants. */
    size_t llen = le - ls;
    if (!is_heading_font(a, lf)) {
        unsigned char* st = styles_for(llen);
        if (target_in_fence) memset(st, ES_CODE, llen);
        else                 compute_edit_styles(a->buf.data + ls, llen, st);
        int x = 0;
        size_t i = 0;
        while (i < llen) {
            unsigned char s = st[i];
            size_t j = i + 1;
            while (j < llen && st[j] == s) j++;
            Font* f = pick_edit_inline_font_for(a, lf, s);
            int seg_w = font_measure(f, a->buf.data + ls + i, j - i);
            if (x + seg_w >= local_x) {
                /* The click is inside this segment — find the byte. */
                size_t k = i;
                int xx = x;
                while (k < j) {
                    size_t nxt = k + 1;
                    while (nxt < j && ((unsigned char)a->buf.data[ls + nxt]
                                       & 0xC0) == 0x80) nxt++;
                    int cw = font_measure(f, a->buf.data + ls + k, nxt - k);
                    if (xx + cw / 2 >= local_x) return ls + k;
                    xx += cw;
                    k = nxt;
                }
                return ls + j;
            }
            x += seg_w;
            i = j;
        }
        return le;
    }

    /* Heading line: single-font walk (no per-style variants). */
    int x = 0;
    size_t i = ls;
    while (i < le) {
        size_t nxt = i + 1;
        while (nxt < le && ((unsigned char)a->buf.data[nxt] & 0xC0) == 0x80) nxt++;
        int cw = font_measure(lf, a->buf.data + i, nxt - i);
        if (x + cw / 2 >= local_x) return i;
        x += cw;
        i = nxt;
    }
    return le;
}

/* ----------------------------- quick switcher --------------------------- */

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int strieq(const char* a, const char* b)
{
    while (*a && *b) {
        if (ascii_lower(*a) != ascii_lower(*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Fuzzy subsequence match: every char of `needle` must appear in `hay`
 * in order, with arbitrary gaps. Case-insensitive. Returns 1/0. */
static int fuzzy_match(const char* hay, const char* needle, size_t nlen)
{
    if (nlen == 0) return 1;
    size_t hl = strlen(hay);
    size_t hi = 0, ni = 0;
    while (ni < nlen && hi < hl) {
        if (ascii_lower(hay[hi]) == ascii_lower(needle[ni])) ni++;
        hi++;
    }
    return ni == nlen;
}

static void switcher_rebuild(App* a)
{
    if (a->switcher_cap < (int)a->vault.count) {
        a->switcher_cap = (int)a->vault.count + 8;
        a->switcher_matches = realloc(a->switcher_matches,
                                      a->switcher_cap * sizeof(int));
    }
    a->switcher_count = 0;
    /* Empty query → show recents first (MRU order), then any other vault
     * file that isn't already in the recent list. Keeps the picker useful
     * the moment it opens, without forcing the user to type. */
    if (a->switcher_qlen == 0 && a->recent_count > 0) {
        for (int r = 0; r < a->recent_count; ++r) {
            int vi = vault_index_of(&a->vault, a->recent_paths[r]);
            if (vi >= 0) a->switcher_matches[a->switcher_count++] = vi;
        }
        for (size_t i = 0; i < a->vault.count; ++i) {
            if (a->vault.items[i].is_dir) continue;
            int already = 0;
            for (int k = 0; k < a->switcher_count; ++k)
                if (a->switcher_matches[k] == (int)i) { already = 1; break; }
            if (!already)
                a->switcher_matches[a->switcher_count++] = (int)i;
        }
    } else {
        for (size_t i = 0; i < a->vault.count; ++i) {
            if (a->vault.items[i].is_dir) continue;
            if (fuzzy_match(a->vault.items[i].name,
                            a->switcher_query, a->switcher_qlen))
                a->switcher_matches[a->switcher_count++] = (int)i;
        }
    }
    a->switcher_selected = a->switcher_count > 0 ? 0 : -1;
}

static void switcher_open(App* a)
{
    a->wc_active         = false;
    a->switcher_active   = true;
    a->switcher_qlen     = 0;
    a->switcher_query[0] = 0;
    switcher_rebuild(a);
    if (!SDL_IsTextInputActive()) SDL_StartTextInput();
}

static void switcher_close(App* a) { a->switcher_active = false; }

static void switcher_select(App* a)
{
    if (a->switcher_selected < 0 ||
        a->switcher_selected >= a->switcher_count) return;
    int vi = a->switcher_matches[a->switcher_selected];
    if (vi < 0 || vi >= (int)a->vault.count) return;
    if (!confirm_discard(a)) return;
    char* path = strdup(a->vault.items[vi].path);
    switcher_close(a);
    load_note(a, path);
    free(path);
}

/* Geometry must match render_switcher exactly. Returns the row index
 * (0..count-1) under (mx, my), or -1 if outside the row band. Mirrors the
 * scroll-window math so a click on a visible row maps to the correct
 * underlying matches[] index. */
static int switcher_row_at(const App* a, int mx, int my)
{
    if (!a->switcher_active) return -1;
    int max_rows = 12;
    int row_h    = font_line_height(a->font_body) + 8;
    int rows     = a->switcher_count < max_rows ? a->switcher_count : max_rows;
    if (rows == 0) return -1;
    int box_w = 560;
    int box_h = row_h * (1 + rows) + 18;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = 90;
    if (mx < box_x || mx >= box_x + box_w) return -1;
    int rows_top = box_y + 8 + row_h;
    if (my < rows_top || my >= rows_top + rows * row_h) return -1;
    if (my >= box_y + box_h) return -1;
    int visible = (my - rows_top) / row_h;
    int start = 0;
    if (a->switcher_count > max_rows) {
        if (a->switcher_selected >= max_rows / 2) {
            start = a->switcher_selected - max_rows / 2;
            if (start + max_rows > a->switcher_count)
                start = a->switcher_count - max_rows;
            if (start < 0) start = 0;
        }
    }
    int row = start + visible;
    if (row < 0 || row >= a->switcher_count) return -1;
    return row;
}

static void render_switcher(App* a)
{
    if (!a->switcher_active) return;

    /* Backdrop dimming the rest. */
    SDL_SetRenderDrawColor(a->renderer, 0, 0, 0, 110);
    SDL_Rect bd = { 0, 0, a->win_w, a->win_h };
    SDL_RenderFillRect(a->renderer, &bd);

    int max_rows = 12;
    int row_h    = font_line_height(a->font_body) + 8;
    int rows     = a->switcher_count < max_rows ? a->switcher_count : max_rows;
    if (rows == 0) rows = 1;
    int box_w = 560;
    int box_h = row_h * (1 + rows) + 18;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = 90;

    SDL_Rect box = { box_x, box_y, box_w, box_h };
    overlay_card(a, box);

    int y = box_y + 8;
    int label_w = font_measure(a->font_body, "Open: ", 6);
    font_draw_line(a->font_body, "Open: ", 6,
                   box_x + 12, y + font_ascent(a->font_body), a->fg_muted);
    font_draw_line(a->font_body, a->switcher_query, a->switcher_qlen,
                   box_x + 12 + label_w, y + font_ascent(a->font_body), a->fg);

    int qw = font_measure(a->font_body, a->switcher_query, a->switcher_qlen);
    SDL_Rect cur = { box_x + 12 + label_w + qw, y, 2,
                     font_line_height(a->font_body) };
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_cursor.r, a->fg_cursor.g, a->fg_cursor.b, 255);
    SDL_RenderFillRect(a->renderer, &cur);

    char info[40];
    snprintf(info, sizeof info, "%d/%zu",
             a->switcher_count, a->vault.count);
    int iw = font_measure(a->font_body, info, strlen(info));
    font_draw_line(a->font_body, info, strlen(info),
                   box_x + box_w - 12 - iw,
                   y + font_ascent(a->font_body), a->fg_muted);

    y += row_h;
    SDL_Rect div = { box_x + 8, y - 2, box_w - 16, 1 };
    SDL_SetRenderDrawColor(a->renderer, 60, 60, 70, 255);
    SDL_RenderFillRect(a->renderer, &div);

    int start = 0;
    if (a->switcher_count > max_rows) {
        if (a->switcher_selected >= max_rows / 2) {
            start = a->switcher_selected - max_rows / 2;
            if (start + max_rows > a->switcher_count)
                start = a->switcher_count - max_rows;
            if (start < 0) start = 0;
        }
    }
    int end = start + max_rows;
    if (end > a->switcher_count) end = a->switcher_count;

    for (int i = start; i < end; ++i) {
        int vi  = a->switcher_matches[i];
        bool sel = (i == a->switcher_selected);
        if (sel) {
            SDL_Rect r = { box_x + 8, y, box_w - 16, row_h };
            SDL_SetRenderDrawColor(a->renderer,
                a->bg_sidebar_active.r, a->bg_sidebar_active.g,
                a->bg_sidebar_active.b, 255);
            SDL_RenderFillRect(a->renderer, &r);
        }
        SDL_Color c = sel ? a->fg_link : a->fg;
        font_draw_line(a->font_body, a->vault.items[vi].name,
                       strlen(a->vault.items[vi].name),
                       box_x + 16,
                       row_text_baseline(a->font_body, y, row_h), c);
        y += row_h;
    }
}

/* ---------------------------- command palette -------------------------- */

/* "ctrl+shift+p" -> "Ctrl+Shift+P". Pretty-prints each '+'-separated token
 * with leading capital. Single-char tokens are upcased; multi-letter ones
 * become Capitalized. Writes into out (cap bytes), always NUL-terminated. */
static void keystr_pretty(const char* in, char* out, size_t cap)
{
    if (!in || !*in) { if (cap) out[0] = 0; return; }
    size_t i = 0;
    size_t j = 0;
    bool   word_start = true;
    while (in[i] && j + 1 < cap) {
        char c = in[i];
        if (c == '+') {
            if (j + 1 < cap) out[j++] = '+';
            word_start = true;
        } else if (word_start) {
            out[j++] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
            word_start = false;
        } else {
            out[j++] = c;
        }
        i++;
    }
    out[j] = 0;
}

/* "save_as" -> "Save As", "vault_search" -> "Vault Search". Underscores are
 * the word separator; everything else is passed through. */
static void cmdp_pretty_label(const char* name, char* out, size_t cap)
{
    if (!name) { if (cap) out[0] = 0; return; }
    size_t i = 0, j = 0;
    bool word_start = true;
    while (name[i] && j + 1 < cap) {
        char c = name[i];
        if (c == '_') {
            if (j + 1 < cap) out[j++] = ' ';
            word_start = true;
        } else if (word_start) {
            out[j++] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
            word_start = false;
        } else {
            out[j++] = c;
        }
        i++;
    }
    out[j] = 0;
}

/* Forward decl: the action table lives further down. */
typedef void (*ActionFn)(App*);
typedef struct {
    const char* name;
    const char* category;
    ActionFn    fn;
} ActionEntry;
static const ActionEntry ACTIONS[];

static void cmdp_entries_reserve(App* a, int extra)
{
    if (a->cmdp_entry_count + extra <= a->cmdp_entry_cap) return;
    int nc = a->cmdp_entry_cap ? a->cmdp_entry_cap * 2 : 64;
    while (nc < a->cmdp_entry_count + extra) nc *= 2;
    a->cmdp_entries = realloc(a->cmdp_entries, nc * sizeof *a->cmdp_entries);
    a->cmdp_entry_cap = nc;
}

static void cmdp_collect_plugin_cb(const char* name, void* ud)
{
    App* a = ud;
    cmdp_entries_reserve(a, 1);
    struct CmdEntry* e = &a->cmdp_entries[a->cmdp_entry_count++];
    snprintf(e->name, sizeof e->name, "%s", name);
    cmdp_pretty_label(name, e->label, sizeof e->label);
    snprintf(e->category, sizeof e->category, "Plugin");
    e->shortcut[0] = 0;
    e->fn          = NULL;
    e->is_plugin   = true;
}

/* Rebuild the entries list from the built-in ACTIONS[] table and the
 * plugin registry. Called every time the palette opens so newly loaded
 * plugins surface without a relaunch. */
static void cmdp_collect(App* a)
{
    a->cmdp_entry_count = 0;
    for (int i = 0; ACTIONS[i].name; ++i) {
        cmdp_entries_reserve(a, 1);
        struct CmdEntry* e = &a->cmdp_entries[a->cmdp_entry_count++];
        snprintf(e->name,     sizeof e->name,     "%s", ACTIONS[i].name);
        snprintf(e->category, sizeof e->category, "%s", ACTIONS[i].category);
        cmdp_pretty_label(ACTIONS[i].name, e->label, sizeof e->label);
        const char* ks = user_kbind_keystr_for_action(ACTIONS[i].name);
        if (ks && *ks) keystr_pretty(ks, e->shortcut, sizeof e->shortcut);
        else           e->shortcut[0] = 0;
        e->fn        = (void(*)(void*))ACTIONS[i].fn;
        e->is_plugin = false;
    }
    /* Append plugin actions; dedupe by name against built-ins. */
    int builtin_n = a->cmdp_entry_count;
    lua_host_each_action(a->lua, cmdp_collect_plugin_cb, a);
    for (int i = builtin_n; i < a->cmdp_entry_count; ) {
        bool dupe = false;
        for (int j = 0; j < builtin_n; ++j) {
            if (strcmp(a->cmdp_entries[i].name,
                       a->cmdp_entries[j].name) == 0) { dupe = true; break; }
        }
        if (dupe) {
            for (int k = i; k < a->cmdp_entry_count - 1; ++k)
                a->cmdp_entries[k] = a->cmdp_entries[k + 1];
            a->cmdp_entry_count--;
        } else {
            i++;
        }
    }
}

static int fuzzy_match(const char* hay, const char* needle, size_t nlen);

/* Bring a selected row into view by adjusting cmdp_scroll. Called when the
 * keyboard selection moves; mouse hover does NOT call this so the list
 * doesn't auto-pan under the cursor. */
static int cmdp_row_h(const App* a) { return font_line_height(a->font_body) + 8; }
static int cmdp_max_rows(void)      { return 12; }
static int cmdp_max_scroll(const App* a)
{
    int rh = cmdp_row_h(a);
    int max_h = cmdp_max_rows() * rh;
    int content_h = a->cmdp_count * rh;
    int max_sc = content_h - max_h;
    if (max_sc < 0) max_sc = 0;
    return max_sc;
}
static void cmdp_clamp_scroll(App* a)
{
    int max_sc = cmdp_max_scroll(a);
    if (a->cmdp_scroll < 0) a->cmdp_scroll = 0;
    if (a->cmdp_scroll > max_sc) a->cmdp_scroll = max_sc;
}
static void cmdp_ensure_selected_visible(App* a)
{
    if (a->cmdp_selected < 0) return;
    int rh = cmdp_row_h(a);
    int sel_top = a->cmdp_selected * rh;
    int sel_bot = sel_top + rh;
    int view_h  = cmdp_max_rows() * rh;
    if (sel_top < a->cmdp_scroll)         a->cmdp_scroll = sel_top;
    else if (sel_bot > a->cmdp_scroll + view_h) a->cmdp_scroll = sel_bot - view_h;
    cmdp_clamp_scroll(a);
}

static void cmdp_rebuild(App* a)
{
    if (a->cmdp_cap < a->cmdp_entry_count) {
        a->cmdp_cap = a->cmdp_entry_count + 8;
        a->cmdp_matches = realloc(a->cmdp_matches,
                                  a->cmdp_cap * sizeof(int));
    }
    a->cmdp_count = 0;
    for (int i = 0; i < a->cmdp_entry_count; ++i) {
        const char* L = a->cmdp_entries[i].label;
        const char* N = a->cmdp_entries[i].name;
        const char* C = a->cmdp_entries[i].category;
        if (a->cmdp_qlen == 0
            || fuzzy_match(L, a->cmdp_query, a->cmdp_qlen)
            || fuzzy_match(N, a->cmdp_query, a->cmdp_qlen)
            || fuzzy_match(C, a->cmdp_query, a->cmdp_qlen))
        {
            a->cmdp_matches[a->cmdp_count++] = i;
        }
    }
    a->cmdp_selected = a->cmdp_count > 0 ? 0 : -1;
    a->cmdp_scroll   = 0;
    a->cmdp_hover    = -1;
}

static void cmdp_open(App* a)
{
    a->cmdp_active   = true;
    a->cmdp_qlen     = 0;
    a->cmdp_query[0] = 0;
    a->cmdp_hover    = -1;
    a->cmdp_scroll   = 0;
    cmdp_collect(a);
    cmdp_rebuild(a);
    if (!SDL_IsTextInputActive()) SDL_StartTextInput();
}

static void cmdp_close(App* a) { a->cmdp_active = false; }

static void cmdp_invoke_index(App* a, int idx)
{
    if (idx < 0 || idx >= a->cmdp_count) return;
    int ei = a->cmdp_matches[idx];
    if (ei < 0 || ei >= a->cmdp_entry_count) return;
    struct CmdEntry e = a->cmdp_entries[ei];      /* snapshot — close clears state */
    cmdp_close(a);
    if (e.is_plugin) {
        if (lua_host_invoke_action(a->lua, e.name) != 0) {
            char msg[160];
            snprintf(msg, sizeof msg, "plugin action failed: %s", e.name);
            app_notify(a, msg);
        }
    } else if (e.fn) {
        ((ActionFn)e.fn)(a);
    }
}

static void cmdp_invoke(App* a) { cmdp_invoke_index(a, a->cmdp_selected); }

/* Geometry: returns the row band rect (where rows draw) and box rect. */
static void cmdp_geom(const App* a, SDL_Rect* box_out,
                      SDL_Rect* rows_out, SDL_Rect* track_out,
                      SDL_Rect* thumb_out)
{
    int rh   = cmdp_row_h(a);
    int max_rows = cmdp_max_rows();
    int rows = a->cmdp_count < max_rows ? a->cmdp_count : max_rows;
    if (rows == 0) rows = 1;
    int box_w = 620;
    int box_h = rh * (1 + rows) + 18;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = 90;
    SDL_Rect box = { box_x, box_y, box_w, box_h };
    SDL_Rect rows_r = { box_x + 8, box_y + 8 + rh,
                        box_w - 16, rows * rh };
    if (box_out)  *box_out  = box;
    if (rows_out) *rows_out = rows_r;
    if (track_out || thumb_out) {
        SDL_Rect t, th;
        int content_h = a->cmdp_count * rh;
        if (content_h <= rows_r.h) {
            if (track_out) *track_out = (SDL_Rect){0,0,0,0};
            if (thumb_out) *thumb_out = (SDL_Rect){0,0,0,0};
        } else if (overlay_list_scrollbar_geom(box_x, box_w,
                       rows_r.y, rows_r.y + rows_r.h,
                       content_h, a->cmdp_scroll, &t, &th))
        {
            if (track_out) *track_out = t;
            if (thumb_out) *thumb_out = th;
        }
    }
}

/* Returns the entry index under (mx, my), or -1. Uses scroll-based layout. */
static int cmdp_row_at(const App* a, int mx, int my)
{
    if (!a->cmdp_active) return -1;
    SDL_Rect rows_r;
    cmdp_geom(a, NULL, &rows_r, NULL, NULL);
    /* Exclude the scrollbar gutter so hover doesn't fall through. */
    SDL_Rect track = {0, 0, 0, 0};
    cmdp_geom(a, NULL, NULL, &track, NULL);
    int rows_right = rows_r.x + rows_r.w;
    if (track.w > 0 && track.x < rows_right) rows_right = track.x;
    if (mx < rows_r.x || mx >= rows_right) return -1;
    if (my < rows_r.y || my >= rows_r.y + rows_r.h) return -1;
    int rh = cmdp_row_h(a);
    int row = (my - rows_r.y + a->cmdp_scroll) / rh;
    if (row < 0 || row >= a->cmdp_count) return -1;
    return row;
}

static void render_cmdp(App* a)
{
    if (!a->cmdp_active) return;

    SDL_SetRenderDrawColor(a->renderer, 0, 0, 0, 110);
    SDL_Rect bd = { 0, 0, a->win_w, a->win_h };
    SDL_RenderFillRect(a->renderer, &bd);

    int max_rows = 12;
    int row_h    = font_line_height(a->font_body) + 8;
    int rows     = a->cmdp_count < max_rows ? a->cmdp_count : max_rows;
    if (rows == 0) rows = 1;
    int box_w = 620;
    int box_h = row_h * (1 + rows) + 18;
    int box_x = (a->win_w - box_w) / 2;
    int box_y = 90;

    SDL_Rect box = { box_x, box_y, box_w, box_h };
    overlay_card(a, box);

    /* Prompt + query input. */
    int y = box_y + 8;
    const char* prompt = "> ";
    int label_w = font_measure(a->font_body, prompt, strlen(prompt));
    font_draw_line(a->font_body, prompt, strlen(prompt),
                   box_x + 12, y + font_ascent(a->font_body), a->fg_muted);
    if (a->cmdp_qlen > 0) {
        font_draw_line(a->font_body, a->cmdp_query, a->cmdp_qlen,
                       box_x + 12 + label_w,
                       y + font_ascent(a->font_body), a->fg);
    } else {
        const char* ph = "Run a command...";
        font_draw_line(a->font_body, ph, strlen(ph),
                       box_x + 12 + label_w,
                       y + font_ascent(a->font_body), a->fg_muted);
    }
    int qw = font_measure(a->font_body, a->cmdp_query, a->cmdp_qlen);
    SDL_Rect cur = { box_x + 12 + label_w + qw, y, 2,
                     font_line_height(a->font_body) };
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_cursor.r, a->fg_cursor.g, a->fg_cursor.b, 255);
    SDL_RenderFillRect(a->renderer, &cur);

    char info[40];
    snprintf(info, sizeof info, "%d/%d", a->cmdp_count, a->cmdp_entry_count);
    int iw = font_measure(a->font_body, info, strlen(info));
    font_draw_line(a->font_body, info, strlen(info),
                   box_x + box_w - 12 - iw,
                   y + font_ascent(a->font_body), a->fg_muted);

    y += row_h;
    SDL_Rect div = { box_x + 8, y - 2, box_w - 16, 1 };
    SDL_SetRenderDrawColor(a->renderer, 60, 60, 70, 255);
    SDL_RenderFillRect(a->renderer, &div);

    /* Row band: scroll-based, clipped. Hover is a separate, lighter
     * highlight from the keyboard-driven selection. */
    SDL_Rect rows_r;
    cmdp_geom(a, NULL, &rows_r, NULL, NULL);
    SDL_RenderSetClipRect(a->renderer, &rows_r);

    int first = a->cmdp_scroll / row_h;
    int last  = (a->cmdp_scroll + rows_r.h - 1) / row_h;
    if (first < 0) first = 0;
    if (last >= a->cmdp_count) last = a->cmdp_count - 1;

    for (int i = first; i <= last; ++i) {
        int ei = a->cmdp_matches[i];
        const struct CmdEntry* e = &a->cmdp_entries[ei];
        bool sel = (i == a->cmdp_selected);
        bool hov = (i == a->cmdp_hover) && !sel;
        int row_y = rows_r.y + i * row_h - a->cmdp_scroll;
        SDL_Rect r = { rows_r.x, row_y, rows_r.w, row_h };
        if (sel) {
            SDL_SetRenderDrawColor(a->renderer,
                a->bg_sidebar_active.r, a->bg_sidebar_active.g,
                a->bg_sidebar_active.b, 255);
            SDL_RenderFillRect(a->renderer, &r);
        } else if (hov) {
            SDL_SetRenderDrawColor(a->renderer,
                a->bg_sidebar_hover.r, a->bg_sidebar_hover.g,
                a->bg_sidebar_hover.b, 140);
            SDL_RenderFillRect(a->renderer, &r);
        }
        int  y = row_y;     /* shadowed for the chip math below */
        SDL_Color c = sel ? a->fg_link : a->fg;
        font_draw_line(a->font_body, e->label, strlen(e->label),
                       box_x + 16,
                       row_text_baseline(a->font_body, y, row_h), c);
        /* Right-aligned chips: shortcut, category. */
        int rx = box_x + box_w - 16;
        if (e->shortcut[0]) {
            int sw = font_measure(a->font_body, e->shortcut,
                                  strlen(e->shortcut));
            rx -= sw;
            font_draw_line(a->font_body, e->shortcut, strlen(e->shortcut),
                           rx, row_text_baseline(a->font_body, y, row_h),
                           a->fg_muted);
            rx -= 16;
        }
        if (e->category[0]) {
            int cw = font_measure(a->font_body, e->category,
                                  strlen(e->category));
            int chip_pad = 8;
            int chip_w = cw + chip_pad * 2;
            int chip_h = row_h - 10;
            int chip_x = rx - chip_w;
            int chip_y = y + (row_h - chip_h) / 2;
            SDL_Color chip_bg = e->is_plugin ? a->fg_link
                                             : a->bg_sidebar_hover;
            SDL_SetRenderDrawColor(a->renderer, chip_bg.r, chip_bg.g,
                                   chip_bg.b, e->is_plugin ? 220 : 180);
            fill_rrect(a->renderer,
                       (SDL_Rect){chip_x, chip_y, chip_w, chip_h},
                       chip_h / 2);
            SDL_Color chip_fg = a->fg;
            if (e->is_plugin) {
                int lum = a->fg_link.r * 30 + a->fg_link.g * 59
                        + a->fg_link.b * 11;
                chip_fg = (lum > 12000) ? (SDL_Color){20, 20, 26, 255}
                                        : (SDL_Color){240, 240, 250, 255};
            }
            font_draw_line(a->font_body, e->category, strlen(e->category),
                           chip_x + chip_pad,
                           row_text_baseline(a->font_body, y, row_h),
                           chip_fg);
        }
    }
    SDL_RenderSetClipRect(a->renderer, NULL);

    /* Scrollbar with arrows. Hidden if all rows fit. */
    SDL_Rect track, thumb;
    cmdp_geom(a, NULL, NULL, &track, &thumb);
    if (track.w > 0) {
        overlay_scrollbar_draw(a, &track, &thumb, a->sb_drag == SB_CMDP);
    }
}

/* ----------------------------- plugins overlay ------------------------- */

static void plugins_rows_reserve(App* a, int extra)
{
    if (a->plugins_count + extra <= a->plugins_cap) return;
    int nc = a->plugins_cap ? a->plugins_cap * 2 : 8;
    while (nc < a->plugins_count + extra) nc *= 2;
    a->plugins_rows = realloc(a->plugins_rows,
                              nc * sizeof *a->plugins_rows);
    a->plugins_cap  = nc;
}

static void plugins_collect_cb(const LuaPluginView* p, void* ud)
{
    App* a = ud;
    plugins_rows_reserve(a, 1);
    struct PluginRow* r = &a->plugins_rows[a->plugins_count++];
    memset(r, 0, sizeof *r);
    snprintf(r->name,   sizeof r->name,   "%s", p->name ? p->name : "?");
    snprintf(r->path,   sizeof r->path,   "%s", p->path ? p->path : "");
    if (p->load_failed) {
        snprintf(r->status, sizeof r->status, "error");
        snprintf(r->error,  sizeof r->error,  "%s", p->error ? p->error : "");
    } else {
        snprintf(r->status, sizeof r->status, "loaded");
    }
    int n = p->action_count;
    int cap = (int)(sizeof r->actions / sizeof r->actions[0]);
    if (n > cap) n = cap;
    for (int i = 0; i < n; ++i) {
        snprintf(r->actions[i], sizeof r->actions[0], "%s",
                 p->actions[i] ? p->actions[i] : "");
    }
    r->action_count = n;
}

static void plugins_collect(App* a)
{
    a->plugins_count = 0;
    lua_host_each_plugin(a->lua, plugins_collect_cb, a);
}

static void plugins_open(App* a)
{
    plugins_collect(a);
    a->plugins_active   = true;
    a->plugins_selected = a->plugins_count > 0 ? 0 : -1;
    a->plugins_scroll   = 0;
}

static void plugins_close(App* a) { a->plugins_active = false; }

static SDL_Rect plugins_box_rect(const App* a)
{
    int w = 680;
    int h = 480;
    if (h > a->win_h - 80) h = a->win_h - 80;
    return (SDL_Rect){ (a->win_w - w) / 2, (a->win_h - h) / 2, w, h };
}

static int plugins_row_h(const App* a)
{
    return font_line_height(a->font_body) + 8;
}

static int plugins_reload_btn(const App* a, SDL_Rect* out)
{
    SDL_Rect box = plugins_box_rect(a);
    int sz_y     = font_line_height(a->font_body);
    int header_h = sz_y + 24;
    int btn_h    = sz_y + 10;
    int btn_w    = 110;
    int btn_x    = box.x + box.w - btn_w - 16;
    int btn_y    = box.y + (header_h - btn_h) / 2;
    if (out) *out = (SDL_Rect){ btn_x, btn_y, btn_w, btn_h };
    return 0;
}

static void plugins_action_reload(App* a)
{
    const char* pdir = lua_host_cfg_string(a->lua, "plugin_path",
                                           "data/plugins");
    int n = lua_host_reload_plugins(a->lua, pdir);
    plugins_collect(a);
    char msg[160];
    snprintf(msg, sizeof msg, "reloaded %d plugin(s) from %s", n, pdir);
    app_notify(a, msg);
}

/* Truncate `in` to fit `max_w` pixels, writing result to `out`. If the
 * full string doesn't fit, the START is replaced with "…" so the
 * file's basename (the useful part) stays visible. */
static void path_fit_left(Font* f, const char* in, int max_w,
                          char* out, size_t cap)
{
    size_t n = strlen(in);
    if ((int)font_measure(f, in, n) <= max_w) {
        snprintf(out, cap, "%s", in);
        return;
    }
    const char* ell = "...";
    int ell_w = font_measure(f, ell, strlen(ell));
    int budget = max_w - ell_w;
    if (budget < 0) budget = 0;
    /* Walk from the END backward — keep as many trailing bytes as fit. */
    size_t keep = 0;
    int    w    = 0;
    for (size_t i = n; i > 0; ) {
        size_t j = i - 1;
        /* Step over UTF-8 continuation bytes so we don't split a codepoint. */
        while (j > 0 && ((unsigned char)in[j] & 0xC0) == 0x80) j--;
        int cw = font_measure(f, in + j, i - j);
        if (w + cw > budget) break;
        w += cw;
        keep = n - j;
        i = j;
    }
    if (keep == n) snprintf(out, cap, "%s", in);
    else snprintf(out, cap, "%s%s", ell, in + (n - keep));
}

static void render_plugins(App* a)
{
    if (!a->plugins_active) return;

    overlay_backdrop(a);
    SDL_Rect box = plugins_box_rect(a);
    overlay_card(a, box);

    int sz_y = font_line_height(a->font_body);
    int header_h = sz_y + 24;

    /* ----- Header row ----- */
    int header_baseline = box.y + 14 + font_ascent(a->font_body);
    /* Reload button — rightmost of the header. */
    int btn_h = sz_y + 10;
    int btn_w = 110;
    int btn_x = box.x + box.w - btn_w - 16;
    int btn_y = box.y + (header_h - btn_h) / 2;
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_link.r, a->fg_link.g, a->fg_link.b, 220);
    fill_rrect(a->renderer, (SDL_Rect){btn_x, btn_y, btn_w, btn_h}, btn_h / 2);
    {
        const char* lab = "Reload";
        int lw = font_measure(a->font_body, lab, strlen(lab));
        int lum = a->fg_link.r * 30 + a->fg_link.g * 59 + a->fg_link.b * 11;
        SDL_Color tc = (lum > 12000) ? (SDL_Color){20, 20, 26, 255}
                                     : (SDL_Color){240, 240, 250, 255};
        font_draw_line(a->font_body, lab, strlen(lab),
                       btn_x + (btn_w - lw) / 2,
                       btn_y + (btn_h - sz_y) / 2 + font_ascent(a->font_body),
                       tc);
    }
    /* "X loaded" — sits just left of the button. */
    char info[64];
    snprintf(info, sizeof info, "%d loaded", a->plugins_count);
    int iw = font_measure(a->font_body, info, strlen(info));
    font_draw_line(a->font_body, info, strlen(info),
                   btn_x - 16 - iw, header_baseline, a->fg_muted);
    /* Title on the left. */
    const char* title = "Plugins";
    font_draw_line(a->font_body, title, strlen(title),
                   box.x + 20, header_baseline, a->fg_link);

    /* Hairline divider under the header. */
    SDL_SetRenderDrawColor(a->renderer, 60, 60, 70, 200);
    SDL_Rect div = { box.x + 12, box.y + header_h, box.w - 24, 1 };
    SDL_RenderFillRect(a->renderer, &div);

    /* ----- Body ----- */
    int list_top = box.y + header_h + 10;
    int list_bot = box.y + box.h - 16;
    SDL_Rect clip = { box.x, list_top, box.w, list_bot - list_top };
    SDL_RenderSetClipRect(a->renderer, &clip);

    int y  = list_top - a->plugins_scroll;
    int rh = plugins_row_h(a);

    if (a->plugins_count == 0) {
        const char* empty = "no plugins loaded — drop a *.lua file in data/plugins/";
        font_draw_line(a->font_body, empty, strlen(empty),
                       box.x + 20, y + font_ascent(a->font_body),
                       a->fg_muted);
        SDL_RenderSetClipRect(a->renderer, NULL);
        return;
    }

    for (int i = 0; i < a->plugins_count; ++i) {
        const struct PluginRow* p = &a->plugins_rows[i];
        bool failed = (p->status[0] == 'e');
        SDL_Color name_c = failed ? (SDL_Color){230, 110, 110, 255} : a->fg_link;
        int baseline = y + font_ascent(a->font_body);

        /* Plugin header row: bold name, status pill on the right. */
        font_draw_line(a->font_body_bold ? a->font_body_bold : a->font_body,
                       p->name, strlen(p->name),
                       box.x + 20, baseline, name_c);
        int nw = font_measure(a->font_body_bold ? a->font_body_bold
                                                : a->font_body,
                              p->name, strlen(p->name));
        /* Status chip next to name. */
        int sw = font_measure(a->font_body, p->status, strlen(p->status));
        int chip_pad_x = 8;
        int chip_w = sw + chip_pad_x * 2;
        int chip_h = rh - 6;
        int chip_x = box.x + 20 + nw + 12;
        int chip_y = y + (rh - chip_h) / 2;
        SDL_Color chip_bg = failed ? (SDL_Color){80, 30, 30, 220}
                                   : a->bg_sidebar_hover;
        SDL_SetRenderDrawColor(a->renderer, chip_bg.r, chip_bg.g, chip_bg.b,
                               failed ? 220 : 180);
        fill_rrect(a->renderer,
                   (SDL_Rect){chip_x, chip_y, chip_w, chip_h}, chip_h / 2);
        font_draw_line(a->font_body, p->status, strlen(p->status),
                       chip_x + chip_pad_x,
                       row_text_baseline(a->font_body, chip_y, chip_h),
                       failed ? (SDL_Color){240, 200, 200, 255} : a->fg);
        y += rh;

        /* Path row (muted, smaller indent so it visually nests under name).
         * Truncated with leading "..." so the filename stays visible. */
        char shown[512];
        int  max_pw = box.w - 36 - 20;
        path_fit_left(a->font_body, p->path, max_pw, shown, sizeof shown);
        font_draw_line(a->font_body, shown, strlen(shown),
                       box.x + 36,
                       y + font_ascent(a->font_body), a->fg_muted);
        y += rh;

        /* Error message, if any. */
        if (p->error[0]) {
            char err_shown[300];
            path_fit_left(a->font_body, p->error, max_pw,
                          err_shown, sizeof err_shown);
            font_draw_line(a->font_body, err_shown, strlen(err_shown),
                           box.x + 36,
                           y + font_ascent(a->font_body),
                           (SDL_Color){230, 110, 110, 255});
            y += rh;
        }

        /* Action rows, indented with a bullet. */
        if (p->action_count == 0 && !failed) {
            const char* none = "(no actions registered)";
            font_draw_line(a->font_body, none, strlen(none),
                           box.x + 36,
                           y + font_ascent(a->font_body), a->fg_muted);
            y += rh;
        }
        for (int ai = 0; ai < p->action_count; ++ai) {
            const char* dot = "- ";
            font_draw_line(a->font_body, dot, strlen(dot),
                           box.x + 36, y + font_ascent(a->font_body),
                           a->fg_muted);
            int dw = font_measure(a->font_body, dot, strlen(dot));
            font_draw_line(a->font_body, p->actions[ai],
                           strlen(p->actions[ai]),
                           box.x + 36 + dw,
                           y + font_ascent(a->font_body), a->fg);
            y += rh;
        }

        /* Spacing between plugins. */
        y += 8;
    }

    SDL_RenderSetClipRect(a->renderer, NULL);
}

static int plugins_reload_hit(const App* a, int mx, int my)
{
    SDL_Rect br;
    plugins_reload_btn(a, &br);
    if (mx >= br.x && mx < br.x + br.w &&
        my >= br.y && my < br.y + br.h) return 1;
    return 0;
}

/* Compute screen (x, y) of the cursor in edit mode. y is the TOP of the
 * cursor caret rect; x is just to the right of the cursor's current byte
 * position. Used to anchor in-editor popovers (wiki-complete). */
static void edit_cursor_screen_pos(const App* a, int* out_x, int* out_y)
{
    int xL = doc_x_left(a);
    int y  = doc_y_top(a) - a->scroll_y;
    Buffer* b = (Buffer*)&a->buf;
    size_t n_lines = buffer_line_count(b);
    bool   in_fence = false;
    for (size_t li = 0; li < n_lines; ++li) {
        size_t ls   = buffer_line_start(b, li);
        size_t le   = buffer_line_end  (b, li);
        size_t llen = le - ls;
        Font*  lf   = edit_line_font(a, b->data + ls, llen);
        int    lh   = line_step(a, lf);
        bool line_is_fence = is_fence_line(b->data + ls, llen);
        bool draw_in_fence = in_fence || line_is_fence;
        if (b->cursor >= ls && b->cursor <= le) {
            int cx = xL + MARGIN_X +
                edit_line_x_at(a, b->data + ls, llen, b->cursor - ls,
                               lf, draw_in_fence);
            if (out_x) *out_x = cx;
            if (out_y) *out_y = y + lh;       /* below the caret */
            return;
        }
        if (line_is_fence) in_fence = !in_fence;
        y += lh;
    }
    if (out_x) *out_x = xL + MARGIN_X;
    if (out_y) *out_y = y;
}

/* ----------------------------- wiki-link auto-complete ------------------ */

/* Display name for a wiki-link target: vault item name without `.md` suffix.
 * Returns the byte length to use; stores result in `out` (caller-sized). */
static size_t wc_display_name(const char* name, char* out, size_t out_cap)
{
    size_t n = strlen(name);
    if (n > 3 && (name[n-3] == '.') &&
        (name[n-2] == 'm' || name[n-2] == 'M') &&
        (name[n-1] == 'd' || name[n-1] == 'D')) n -= 3;
    if (n >= out_cap) n = out_cap - 1;
    memcpy(out, name, n);
    out[n] = 0;
    return n;
}

static void wc_rebuild(App* a)
{
    if (a->wc_cap < (int)a->vault.count) {
        a->wc_cap = (int)a->vault.count + 8;
        a->wc_matches = realloc(a->wc_matches, a->wc_cap * sizeof(int));
    }
    a->wc_count = 0;
    /* Filter is the buffer slice between anchor and cursor. */
    const char* q     = a->buf.data + a->wc_anchor;
    size_t      qlen  = (a->buf.cursor > a->wc_anchor)
                        ? a->buf.cursor - a->wc_anchor : 0;
    for (size_t i = 0; i < a->vault.count; ++i) {
        if (a->vault.items[i].is_dir) continue;
        char disp[256];
        wc_display_name(a->vault.items[i].name, disp, sizeof disp);
        if (fuzzy_match(disp, q, qlen))
            a->wc_matches[a->wc_count++] = (int)i;
    }
    if (a->wc_selected >= a->wc_count) a->wc_selected = a->wc_count - 1;
    if (a->wc_selected < 0 && a->wc_count > 0) a->wc_selected = 0;
}

static void wc_open_at_cursor(App* a, int screen_x, int screen_y)
{
    a->wc_active   = true;
    a->wc_anchor   = a->buf.cursor;
    a->wc_selected = 0;
    a->wc_x = screen_x;
    a->wc_y = screen_y;
    wc_rebuild(a);
}

static void wc_close(App* a) { a->wc_active = false; }

/* Insert the selected name (without `.md`) into the buffer, replacing
 * whatever the user typed between wc_anchor and cursor. The surrounding
 * `[[` and `]]` are already in place from the auto-pair, so just swap
 * the middle. */
static void wc_select(App* a)
{
    if (a->wc_selected < 0 || a->wc_selected >= a->wc_count) {
        wc_close(a); return;
    }
    int vi = a->wc_matches[a->wc_selected];
    if (vi < 0 || vi >= (int)a->vault.count) { wc_close(a); return; }

    char disp[256];
    size_t dn = wc_display_name(a->vault.items[vi].name, disp, sizeof disp);

    /* Delete chars between anchor and cursor, then insert the display name. */
    size_t typed = (a->buf.cursor > a->wc_anchor)
                   ? a->buf.cursor - a->wc_anchor : 0;
    for (size_t i = 0; i < typed; ++i) buffer_delete_back(&a->buf);
    buffer_insert(&a->buf, disp, dn);
    /* Skip past the auto-paired `]]` so the cursor lands after the link. */
    if (a->buf.cursor + 2 <= a->buf.len &&
        a->buf.data[a->buf.cursor]     == ']' &&
        a->buf.data[a->buf.cursor + 1] == ']')
    {
        a->buf.cursor += 2;
        a->buf.sel_anchor = -1;
    }
    wc_close(a);
}

static void render_wiki_complete(App* a)
{
    if (!a->wc_active) return;
    if (!a->edit_mode || a->switcher_active || a->search_mode != 0) return;

    int row_h    = font_line_height(a->font_body) + 6;
    int max_rows = 8;
    int rows     = a->wc_count < max_rows ? a->wc_count : max_rows;
    if (rows == 0) rows = 1;

    int box_w = 320;
    int box_h = row_h * rows + 12;
    int box_x = a->wc_x;
    int box_y = a->wc_y;
    if (box_x + box_w > a->win_w - 8) box_x = a->win_w - 8 - box_w;
    if (box_x < 8) box_x = 8;
    if (box_y + box_h > a->win_h - status_bar_h(a) - 4)
        box_y = a->wc_y - box_h - font_line_height(a->font_code);
    if (box_y < 8) box_y = 8;

    SDL_Rect box = { box_x, box_y, box_w, box_h };
    overlay_card(a, box);

    if (a->wc_count == 0) {
        const char* msg = "No matching notes";
        font_draw_line(a->font_body, msg, strlen(msg),
                       box_x + 12, box_y + 6 + font_ascent(a->font_body),
                       a->fg_muted);
        return;
    }

    int start = 0;
    if (a->wc_count > max_rows) {
        if (a->wc_selected >= max_rows / 2) {
            start = a->wc_selected - max_rows / 2;
            if (start + max_rows > a->wc_count) start = a->wc_count - max_rows;
            if (start < 0) start = 0;
        }
    }
    int end = start + max_rows;
    if (end > a->wc_count) end = a->wc_count;

    int y = box_y + 6;
    for (int i = start; i < end; ++i) {
        int  vi  = a->wc_matches[i];
        bool sel = (i == a->wc_selected);
        if (sel) {
            SDL_Rect r = { box_x + 4, y, box_w - 8, row_h };
            SDL_SetRenderDrawColor(a->renderer,
                a->bg_sidebar_active.r, a->bg_sidebar_active.g,
                a->bg_sidebar_active.b, 255);
            SDL_RenderFillRect(a->renderer, &r);
        }
        char disp[256];
        size_t dn = wc_display_name(a->vault.items[vi].name, disp, sizeof disp);
        SDL_Color c = sel ? a->fg_link : a->fg;
        font_draw_line(a->font_body, disp, dn,
                       box_x + 10, y + font_ascent(a->font_body) + 2, c);
        y += row_h;
    }
}

/* ----------------------------- find / replace --------------------------- */

static void search_clear_matches(App* a)
{
    free(a->search_matches);
    free(a->search_match_lens);
    a->search_matches     = NULL;
    a->search_match_lens  = NULL;
    a->search_count       = 0;
    a->search_cap         = 0;
    a->search_current     = -1;
}

/* Append (start, len) to the matches arrays, growing as needed. */
static void search_append_match(App* a, size_t start, size_t len)
{
    if (a->search_count >= a->search_cap) {
        a->search_cap = a->search_cap ? a->search_cap * 2 : 16;
        a->search_matches    = realloc(a->search_matches,
                                       a->search_cap * sizeof(size_t));
        a->search_match_lens = realloc(a->search_match_lens,
                                       a->search_cap * sizeof(size_t));
    }
    a->search_matches   [a->search_count] = start;
    a->search_match_lens[a->search_count] = len;
    a->search_count++;
}

/* Compare `q[0..qlen)` against `data + i` honoring case-insensitive flag. */
static int search_match_at(const App* a, const char* data, size_t i, size_t qlen)
{
    if (a->search_case_insensitive) {
        for (size_t k = 0; k < qlen; ++k) {
            char x = data[i + k], y = a->search_query[k];
            if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
            if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
            if (x != y) return 0;
        }
        return 1;
    }
    return memcmp(data + i, a->search_query, qlen) == 0;
}

/* True if byte `c` is part of an identifier-like word (used for whole-word). */
static int search_is_word_byte(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80;
}

static void search_rebuild(App* a)
{
    search_clear_matches(a);
    a->search_re_err[0] = 0;
    if (a->search_qlen == 0) return;
    /* CRITICAL: the highlight overlay in preview uses doc-byte offsets
     * (because each preview run only knows its position inside doc.data),
     * while the edit-mode highlight uses buffer-byte offsets. Searching
     * the WRONG buffer makes preview highlights land on completely
     * unrelated glyphs (the `## ` / `**` / etc. that md4c strips out
     * shift every offset). So we scan whichever buffer is being shown. */
    const char*  data;
    size_t       len;
    if (a->edit_mode) {
        data = a->buf.data;
        len  = a->buf.len;
    } else {
        data = a->doc.data ? a->doc.data : "";
        len  = a->doc.len;
    }

    if (a->search_regex) {
        /* Compile the pattern; on failure leave the error string set so the
         * overlay can show it inline. */
        DsRegex* re = ds_regex_compile(a->search_query,
                                       a->search_case_insensitive,
                                       a->search_re_err,
                                       sizeof a->search_re_err);
        if (!re) return;
        size_t i = 0;
        while (i <= len) {
            size_t s, e;
            if (!ds_regex_find(re, data, len, i, &s, &e)) break;
            size_t mlen = e - s;
            if (a->search_whole_word) {
                int left_ok  = (s == 0) ||
                    !search_is_word_byte((unsigned char)data[s - 1]);
                int right_ok = (e == len) ||
                    !search_is_word_byte((unsigned char)data[e]);
                if (!left_ok || !right_ok) {
                    /* Skip this match but still advance to avoid an infinite
                     * loop on zero-length matches. */
                    i = (mlen == 0) ? s + 1 : s + mlen;
                    continue;
                }
            }
            search_append_match(a, s, mlen);
            i = (mlen == 0) ? s + 1 : s + mlen;
        }
        ds_regex_free(re);
    } else {
        const size_t qlen = a->search_qlen;
        for (size_t i = 0; i + qlen <= len; ++i) {
            if (!search_match_at(a, data, i, qlen)) continue;
            if (a->search_whole_word) {
                int left_ok  = (i == 0) ||
                    !search_is_word_byte((unsigned char)data[i - 1]);
                int right_ok = (i + qlen == len) ||
                    !search_is_word_byte((unsigned char)data[i + qlen]);
                if (!left_ok || !right_ok) continue;
            }
            search_append_match(a, i, qlen);
            i += qlen - 1;
        }
    }
    if (a->search_count > 0) a->search_current = 0;
}

static void search_jump_current(App* a)
{
    if (a->search_current < 0) return;
    size_t pos = a->search_matches[a->search_current];
    size_t len = a->search_match_lens
                 ? a->search_match_lens[a->search_current]
                 : a->search_qlen;
    if (a->edit_mode) {
        buffer_set_cursor(&a->buf, pos, false);
        a->buf.sel_anchor = (long)pos;
        a->buf.cursor     = pos + len;
        ensure_cursor_visible(a);
        bump_blink(a);
    }
}

static void search_next(App* a)
{
    if (a->search_count == 0) return;
    a->search_current = (a->search_current + 1) % (int)a->search_count;
    search_jump_current(a);
}

static void search_prev(App* a)
{
    if (a->search_count == 0) return;
    a->search_current = (a->search_current - 1 + (int)a->search_count)
                        % (int)a->search_count;
    search_jump_current(a);
}

static void search_close(App* a)
{
    a->search_mode = 0;
    a->search_qlen = 0;
    a->search_rlen = 0;
    a->search_qcursor = 0;
    a->search_rcursor = 0;
    a->search_query[0] = 0;
    a->search_replace[0] = 0;
    a->search_focus_replace = false;
    search_clear_matches(a);
}

static void search_replace_one(App* a)
{
    if (a->search_mode != 2) return;
    if (!a->edit_mode) {
        app_notify(a, "switch to edit mode (Ctrl+E) to replace");
        return;
    }
    if (a->search_current < 0) return;
    size_t pos = a->search_matches[a->search_current];
    size_t len = a->search_match_lens
                 ? a->search_match_lens[a->search_current]
                 : a->search_qlen;
    a->buf.sel_anchor = (long)pos;
    a->buf.cursor     = pos + len;
    buffer_undo_break(&a->buf);
    buffer_insert(&a->buf, a->search_replace, a->search_rlen);
    buffer_undo_break(&a->buf);
    update_window_title(a);
    search_rebuild(a);
    /* Re-aim current at the next match at-or-after the replaced spot. */
    size_t target = pos + a->search_rlen;
    a->search_current = -1;
    for (size_t i = 0; i < a->search_count; ++i) {
        if (a->search_matches[i] >= target) { a->search_current = (int)i; break; }
    }
    if (a->search_current < 0 && a->search_count > 0) a->search_current = 0;
    search_jump_current(a);
}

static void search_replace_all(App* a)
{
    if (a->search_mode != 2 || a->search_count == 0) return;
    if (!a->edit_mode) {
        app_notify(a, "switch to edit mode (Ctrl+E) to replace");
        return;
    }
    buffer_undo_break(&a->buf);
    /* Walk matches in reverse so earlier indices stay valid. */
    for (int i = (int)a->search_count - 1; i >= 0; --i) {
        size_t pos = a->search_matches[i];
        size_t len = a->search_match_lens
                     ? a->search_match_lens[i]
                     : a->search_qlen;
        a->buf.sel_anchor = (long)pos;
        a->buf.cursor     = pos + len;
        buffer_insert(&a->buf, a->search_replace, a->search_rlen);
    }
    buffer_undo_break(&a->buf);
    update_window_title(a);
    search_rebuild(a);
}

/* Modern find bar: an inline pill-shaped input field with a leading
 * magnifier icon, a placeholder, mode toggle chips on the right, and a
 * counter pill. Replaces the prior label-then-text strip that looked
 * like a 90s status line. */
/* Compute every clickable region in the search overlay, in one place so
 * render + hit-test stay in agreement. Output pointers may be NULL to
 * skip what the caller doesn't care about. Returns false if search is
 * closed (all rects undefined). */
typedef enum {
    SEARCH_HIT_NONE        = -1,
    SEARCH_HIT_INPUT       = 0,
    SEARCH_HIT_REPLACE     = 1,
    SEARCH_HIT_CHIP_RE     = 2,
    SEARCH_HIT_CHIP_AA     = 3,
    SEARCH_HIT_CHIP_W      = 4,
    SEARCH_HIT_TOGGLE      = 5,
    SEARCH_HIT_BTN_REPL    = 6,   /* Replace one — only in mode 2 */
    SEARCH_HIT_BTN_REPL_ALL= 7,
} SearchHit;

/* Map an x offset (relative to the start of the text — i.e. 0 = right at
 * the first glyph) to a UTF-8-safe byte index in `s`. Used to drop the
 * search caret where the user clicked. */
static size_t text_byte_at_x(Font* f, const char* s, size_t n, int x_in_text)
{
    if (x_in_text <= 0 || n == 0) return 0;
    int acc = 0;
    size_t i = 0;
    while (i < n) {
        size_t j = i + 1;
        while (j < n && ((unsigned char)s[j] & 0xC0) == 0x80) j++;
        int cw = font_measure(f, s + i, j - i);
        if (x_in_text <= acc + cw / 2) return i;
        acc += cw;
        i = j;
    }
    return n;
}

static bool search_geometry(const App* a, SDL_Rect* bar, SDL_Rect* input,
                            SDL_Rect* replace, SDL_Rect chips[3],
                            SDL_Rect* toggle,
                            SDL_Rect* btn_repl, SDL_Rect* btn_repl_all)
{
    if (a->search_mode == 0) return false;
    int xL    = doc_x_left(a);
    int xR    = doc_x_right(a);
    int sz_y  = font_line_height(a->font_body);
    int input_h = sz_y + 14;
    int gap   = 8;
    int rows  = (a->search_mode == 2) ? 2 : 1;
    int h     = input_h * rows + (rows > 1 ? gap : 0) + 16;
    int yT    = chrome_bar_h(a) + 1;

    int chip_pad = 8;
    int chip_h   = input_h - 4;
    int chip_y   = yT + 8 + (input_h - chip_h) / 2;
    int chip_re_w = font_measure(a->font_body, "Re", 2) + 2 * chip_pad;
    int chip_aa_w = font_measure(a->font_body, "Aa", 2) + 2 * chip_pad;
    int chip_w_w  = font_measure(a->font_body, "W",  1) + 2 * chip_pad;
    int chips_total = chip_re_w + chip_aa_w + chip_w_w + 2 * 6 + 12;

    /* Chevron button: square, sits flush left of the input pill. */
    int tog_sz = input_h;
    int tog_x  = xL + 8;
    int tog_y  = yT + 8;

    int input_x = tog_x + tog_sz + 8;
    int input_y = yT + 8;
    int input_w = (xR - 16) - input_x - chips_total;
    if (input_w < 200) input_w = 200;

    if (bar)     *bar     = (SDL_Rect){ xL, yT, xR - xL, h };
    if (input)   *input   = (SDL_Rect){ input_x, input_y, input_w, input_h };
    if (toggle)  *toggle  = (SDL_Rect){ tog_x,   tog_y,   tog_sz,  input_h };
    if (replace) {
        if (a->search_mode == 2)
            *replace = (SDL_Rect){ input_x, input_y + input_h + gap,
                                   input_w, input_h };
        else
            *replace = (SDL_Rect){ 0, 0, 0, 0 };
    }
    if (chips) {
        int cx = input_x + input_w + 12;
        chips[0] = (SDL_Rect){ cx, chip_y, chip_re_w, chip_h };
        cx += chip_re_w + 6;
        chips[1] = (SDL_Rect){ cx, chip_y, chip_aa_w, chip_h };
        cx += chip_aa_w + 6;
        chips[2] = (SDL_Rect){ cx, chip_y, chip_w_w,  chip_h };
    }
    /* Replace / Replace-all buttons live on the replace row's right side,
     * where the chips sit on the find row above. Width is sized to fit the
     * widest label so they never collide. */
    int repl_btn_h = chip_h;
    int repl_pad_h = 14;        /* horizontal pad inside each button       */
    int repl_gap   = 10;        /* gap between the two buttons             */
    int w1 = font_measure(a->font_body, "Replace",     7) + 2 * repl_pad_h;
    int w2 = font_measure(a->font_body, "Replace all", 11) + 2 * repl_pad_h;
    int repl_x_far = input_x + input_w + 12 + chips_total - 12;     /* same right edge as chips */
    int repl_y     = input_y + input_h + gap + (input_h - repl_btn_h) / 2;
    if (btn_repl_all)
        *btn_repl_all = (a->search_mode == 2)
            ? (SDL_Rect){ repl_x_far - w2, repl_y, w2, repl_btn_h }
            : (SDL_Rect){ 0,0,0,0 };
    if (btn_repl)
        *btn_repl = (a->search_mode == 2)
            ? (SDL_Rect){ repl_x_far - w2 - repl_gap - w1,
                          repl_y, w1, repl_btn_h }
            : (SDL_Rect){ 0,0,0,0 };
    return true;
}

static int search_overlay_hit_test(const App* a, int mx, int my)
{
    SDL_Rect input, replace, chips[3], toggle, br, bra;
    if (!search_geometry(a, NULL, &input, &replace, chips, &toggle, &br, &bra))
        return SEARCH_HIT_NONE;
    if (mx >= toggle.x && mx < toggle.x + toggle.w &&
        my >= toggle.y && my < toggle.y + toggle.h) return SEARCH_HIT_TOGGLE;
    if (br.w && mx >= br.x && mx < br.x + br.w &&
        my >= br.y && my < br.y + br.h) return SEARCH_HIT_BTN_REPL;
    if (bra.w && mx >= bra.x && mx < bra.x + bra.w &&
        my >= bra.y && my < bra.y + bra.h) return SEARCH_HIT_BTN_REPL_ALL;
    if (mx >= chips[0].x && mx < chips[0].x + chips[0].w &&
        my >= chips[0].y && my < chips[0].y + chips[0].h) return SEARCH_HIT_CHIP_RE;
    if (mx >= chips[1].x && mx < chips[1].x + chips[1].w &&
        my >= chips[1].y && my < chips[1].y + chips[1].h) return SEARCH_HIT_CHIP_AA;
    if (mx >= chips[2].x && mx < chips[2].x + chips[2].w &&
        my >= chips[2].y && my < chips[2].y + chips[2].h) return SEARCH_HIT_CHIP_W;
    if (mx >= input.x && mx < input.x + input.w &&
        my >= input.y && my < input.y + input.h) return SEARCH_HIT_INPUT;
    if (replace.w && mx >= replace.x && mx < replace.x + replace.w &&
        my >= replace.y && my < replace.y + replace.h) return SEARCH_HIT_REPLACE;
    return SEARCH_HIT_NONE;
}

static bool search_bar_contains(const App* a, int mx, int my)
{
    SDL_Rect bar;
    if (!search_geometry(a, &bar, NULL, NULL, NULL, NULL, NULL, NULL))
        return false;
    return mx >= bar.x && mx < bar.x + bar.w &&
           my >= bar.y && my < bar.y + bar.h;
}

static void render_search_overlay(App* a)
{
    if (a->search_mode == 0) return;

    int xL    = doc_x_left(a);
    int xR    = doc_x_right(a);
    int sz_y  = font_line_height(a->font_body);
    int input_h = sz_y + 14;
    int gap   = 8;
    int rows  = (a->search_mode == 2) ? 2 : 1;
    int h     = input_h * rows + (rows > 1 ? gap : 0) + 16;
    int yT    = chrome_bar_h(a) + 1;

    /* Bar bg + bottom hairline. */
    SDL_Rect bg = { xL, yT, xR - xL, h };
    SDL_SetRenderDrawColor(a->renderer,
        a->bg_status.r, a->bg_status.g, a->bg_status.b, 250);
    SDL_RenderFillRect(a->renderer, &bg);
    SDL_Rect bot = { xL, yT + h, xR - xL, 1 };
    SDL_SetRenderDrawColor(a->renderer,
        a->fg_muted.r, a->fg_muted.g, a->fg_muted.b, 80);
    SDL_RenderFillRect(a->renderer, &bot);

    /* Chevron: toggles between find (1) and find+replace (2). The icon
     * rotates from ▶ (collapsed, mode=1) to ▼ (expanded, mode=2). */
    SDL_Rect tog = {0, 0, 0, 0};
    search_geometry(a, NULL, NULL, NULL, NULL, &tog, NULL, NULL);
    {
        SDL_Color tc = a->fg_muted;
        IconId  ic   = (a->search_mode == 2) ? ICON_CARET_DOWN
                                             : ICON_CARET_RIGHT;
        int ic_sz = tog.h - 6;
        icon_draw(a->renderer, ic,
                  tog.x + (tog.w - ic_sz) / 2,
                  tog.y + (tog.h - ic_sz) / 2,
                  ic_sz, tc);
    }

    /* Chip pills row sits flush right of the input pill. We compute their
     * widths first so we can size the input pill to the remaining space. */
    int chip_pad = 8;
    int chip_h   = input_h - 4;
    int chip_y   = yT + 8 + (input_h - chip_h) / 2;

    /* Helper to measure a chip given its label. */
    #define CHIP_W(label) (font_measure(a->font_body, (label), strlen(label)) + 2 * chip_pad)

    const char* lab_re   = "Re";
    const char* lab_aa   = "Aa";
    const char* lab_w    = "W";
    int chip_re_w = CHIP_W(lab_re);
    int chip_aa_w = CHIP_W(lab_aa);
    int chip_w_w  = CHIP_W(lab_w);
    int chips_total = chip_re_w + chip_aa_w + chip_w_w + 2 * 6 + 12;

    int input_x = tog.x + tog.w + 8;
    int input_y = yT + 8;
    int input_w = (xR - 16) - input_x - chips_total;
    if (input_w < 200) input_w = 200;

    /* Input pill — the search field. Outline + accent if regex error. */
    SDL_Rect input_rect = { input_x, input_y, input_w, input_h };
    bool find_focused = !a->search_focus_replace;
    SDL_SetRenderDrawColor(a->renderer,
        a->bg.r, a->bg.g, a->bg.b, 220);
    fill_rrect(a->renderer, input_rect, input_h / 2);

    SDL_Color border_c =
        (a->search_regex && a->search_re_err[0])
            ? (SDL_Color){230, 110, 110, 200}
            : (find_focused ? a->fg_link : a->fg_muted);
    border_c.a = find_focused ? 200 : 100;
    SDL_SetRenderDrawColor(a->renderer, border_c.r, border_c.g, border_c.b, border_c.a);
    draw_rrect(a->renderer, input_rect, input_h / 2);

    /* Magnifier icon inside the pill. */
    int icon_sz = input_h - 6;
    int icon_x  = input_x + 6;
    int icon_y  = input_y + (input_h - icon_sz) / 2;
    icon_draw(a->renderer, ICON_FIND, icon_x, icon_y, icon_sz, a->fg_muted);

    /* Query text or placeholder. */
    int text_x   = icon_x + icon_sz + 6;
    int text_y   = input_y + (input_h - sz_y) / 2 + font_ascent(a->font_body);
    if (a->search_qlen > 0) {
        font_draw_line(a->font_body, a->search_query, a->search_qlen,
                       text_x, text_y, a->fg);
    } else {
        const char* ph = "Search this note...";
        font_draw_line(a->font_body, ph, strlen(ph),
                       text_x, text_y, a->fg_muted);
    }
    /* Caret in the find field — only when this field is focused, otherwise
     * the user has no idea where their next keystroke will land. */
    if (!a->search_focus_replace) {
        size_t cur = a->search_qcursor;
        if (cur > a->search_qlen) cur = a->search_qlen;
        int cx = text_x + font_measure(a->font_body, a->search_query, cur);
        SDL_Rect caret = { cx, input_y + 4, 2, input_h - 8 };
        SDL_SetRenderDrawColor(a->renderer,
            a->fg_cursor.r, a->fg_cursor.g, a->fg_cursor.b, 230);
        SDL_RenderFillRect(a->renderer, &caret);
    }

    /* Counter (or regex error) — right side INSIDE the pill. */
    char info[96];
    if (a->search_regex && a->search_re_err[0])
        snprintf(info, sizeof info, "%s", a->search_re_err);
    else if (a->search_count > 0)
        snprintf(info, sizeof info, "%d / %zu",
                 a->search_current + 1, a->search_count);
    else if (a->search_qlen > 0)
        snprintf(info, sizeof info, "no match");
    else
        info[0] = 0;
    if (info[0]) {
        int info_w = font_measure(a->font_body, info, strlen(info));
        SDL_Color info_c = (a->search_regex && a->search_re_err[0])
                           ? (SDL_Color){230, 110, 110, 255} : a->fg_muted;
        font_draw_line(a->font_body, info, strlen(info),
                       input_x + input_w - info_w - 12,
                       text_y, info_c);
    }

    /* Mode chip pills, right of the input pill. */
    int cx = input_x + input_w + 12;
    /* Helper macro: draw one chip. */
    #define DRAW_CHIP(label_, w_, on_, err_)                              \
        do {                                                              \
            SDL_Rect _r = { cx, chip_y, w_, chip_h };                     \
            SDL_Color _f = (on_) ? a->fg_link                             \
                                 : (SDL_Color){a->bg_sidebar_hover.r,    \
                                               a->bg_sidebar_hover.g,    \
                                               a->bg_sidebar_hover.b,180}; \
            if (err_) _f = (SDL_Color){230, 110, 110, 220};                \
            SDL_SetRenderDrawColor(a->renderer, _f.r, _f.g, _f.b, _f.a);  \
            fill_rrect(a->renderer, _r, chip_h / 2);                      \
            int _lw = font_measure(a->font_body, (label_), strlen(label_));\
            SDL_Color _tc;                                                 \
            if (on_) {                                                    \
                int _lum = a->fg_link.r * 30 + a->fg_link.g * 59          \
                         + a->fg_link.b * 11;                              \
                _tc = (_lum > 12000) ? (SDL_Color){20, 20, 26, 255}        \
                                     : (SDL_Color){240, 240, 250, 255};   \
            } else _tc = a->fg;                                            \
            font_draw_line(a->font_body, (label_), strlen(label_),         \
                           cx + ((w_) - _lw) / 2,                          \
                           chip_y + (chip_h - sz_y) / 2 +                  \
                               font_ascent(a->font_body),                  \
                           _tc);                                           \
            cx += (w_) + 6;                                                \
        } while (0)

    DRAW_CHIP(lab_re, chip_re_w, a->search_regex,
              a->search_regex && a->search_re_err[0]);
    DRAW_CHIP(lab_aa, chip_aa_w, a->search_case_insensitive, false);
    DRAW_CHIP(lab_w,  chip_w_w,  a->search_whole_word, false);
    #undef DRAW_CHIP
    #undef CHIP_W

    /* Replace row (when search_mode == 2). Same input-pill style. */
    if (a->search_mode == 2) {
        int input_y2 = input_y + input_h + gap;
        SDL_Rect ri = { input_x, input_y2, input_w, input_h };
        SDL_SetRenderDrawColor(a->renderer, a->bg.r, a->bg.g, a->bg.b, 220);
        fill_rrect(a->renderer, ri, input_h / 2);
        SDL_Color rb = a->search_focus_replace ? a->fg_link : a->fg_muted;
        rb.a = a->search_focus_replace ? 200 : 100;
        SDL_SetRenderDrawColor(a->renderer, rb.r, rb.g, rb.b, rb.a);
        draw_rrect(a->renderer, ri, input_h / 2);
        const char* lab = "→";
        font_draw_line(a->font_body, lab, strlen(lab),
                       input_x + 12,
                       input_y2 + (input_h - sz_y) / 2
                                + font_ascent(a->font_body),
                       a->fg_muted);
        int text_x2 = input_x + 12 + font_measure(a->font_body, lab, strlen(lab)) + 8;
        if (a->search_rlen > 0) {
            font_draw_line(a->font_body, a->search_replace, a->search_rlen,
                           text_x2,
                           input_y2 + (input_h - sz_y) / 2
                                    + font_ascent(a->font_body),
                           a->fg);
        } else {
            const char* ph = "Replace with...";
            font_draw_line(a->font_body, ph, strlen(ph),
                           text_x2,
                           input_y2 + (input_h - sz_y) / 2
                                    + font_ascent(a->font_body),
                           a->fg_muted);
        }
        if (a->search_focus_replace) {
            size_t cur = a->search_rcursor;
            if (cur > a->search_rlen) cur = a->search_rlen;
            int cx = text_x2
                + font_measure(a->font_body, a->search_replace, cur);
            SDL_Rect caret = { cx, input_y2 + 4, 2, input_h - 8 };
            SDL_SetRenderDrawColor(a->renderer,
                a->fg_cursor.r, a->fg_cursor.g, a->fg_cursor.b, 230);
            SDL_RenderFillRect(a->renderer, &caret);
        }

        /* Replace / Replace All buttons. Pill-shaped, accent-filled. No
         * `draw_rrect` border — that path is Bresenham (not AA) and looks
         * jagged against the analytically AA'd fill. Hover/enabled state
         * is conveyed by alpha + accent tint instead of a stroke. */
        SDL_Rect br, bra;
        search_geometry(a, NULL, NULL, NULL, NULL, NULL, &br, &bra);
        bool can_replace = a->search_count > 0 && a->search_qlen > 0;
        for (int i = 0; i < 2; ++i) {
            const SDL_Rect r = (i == 0) ? br : bra;
            const char*    L = (i == 0) ? "Replace" : "Replace all";
            /* Right button (Replace All) is the strong action — accent
             * fill. Left button (Replace one) is the soft action. */
            bool primary = (i == 1);
            SDL_Color fill_c;
            if (primary) {
                fill_c = a->fg_link;
                fill_c.a = can_replace ? 220 : 90;
            } else {
                fill_c = a->bg_sidebar_hover;
                fill_c.a = can_replace ? 220 : 130;
            }
            SDL_SetRenderDrawColor(a->renderer,
                fill_c.r, fill_c.g, fill_c.b, fill_c.a);
            fill_rrect(a->renderer, r, r.h / 2);
            int lw = font_measure(a->font_body, L, strlen(L));
            SDL_Color tc;
            if (primary && can_replace) {
                int lum = a->fg_link.r * 30 + a->fg_link.g * 59
                        + a->fg_link.b * 11;
                tc = (lum > 12000) ? (SDL_Color){20, 20, 26, 255}
                                   : (SDL_Color){240, 240, 250, 255};
            } else {
                tc = can_replace ? a->fg : a->fg_muted;
            }
            font_draw_line(a->font_body, L, strlen(L),
                           r.x + (r.w - lw) / 2,
                           row_text_baseline(a->font_body, r.y, r.h),
                           tc);
        }
    }
}

/* ----------------------------- actions ---------------------------------- */
/* User-rebindable commands. Lua's `keybindings` table can map any key chord
 * to one of these by name; otherwise the DEFAULT_KEYS below apply. */

typedef void (*ActionFn)(App*);

/* Forward decl so action_save can fall through to save_as for new buffers. */
static void action_save_as(App* a);

static void action_quit          (App* a) {
    if (confirm_discard(a)) a->running = false;
}
static void action_save          (App* a) {
    if (a->viewing_image) {
        app_notify(a, "image files are view-only");
        return;
    }
    if (!a->note_path || strcmp(a->note_path, "(unsaved)") == 0
                       || strcmp(a->note_path, "(welcome)") == 0) {
        action_save_as(a); return;
    }
    save_note(a);
}
static void action_save_as       (App* a) {
    const char* def = a->note_path ? vault_basename(a->note_path) : "untitled.md";
    if (!def || !*def) def = "untitled.md";
    /* In-app modal: take a filename, resolve relative to vault dir.
     * Replaces the native Win32 save dialog with our own UI. */
    if (!app_text_modal(a, "Save As", def, a->vault.dir)) return;
    if (a->tinput_len == 0) return;
    char picked[1024];
    /* Absolute path? Use as-is. Otherwise place inside the modal's current
     * directory (which reflects any subdir navigation the user did). */
    bool is_abs = (a->tinput_text[0] == '/' || a->tinput_text[0] == '\\' ||
                   (a->tinput_len >= 2 && a->tinput_text[1] == ':'));
    if (is_abs) {
        snprintf(picked, sizeof picked, "%s", a->tinput_text);
    } else if (a->tinput_dir[0]) {
        snprintf(picked, sizeof picked, "%s/%s",
                 a->tinput_dir, a->tinput_text);
    } else {
        snprintf(picked, sizeof picked, "%s", a->tinput_text);
    }
    if (buffer_save(&a->buf, picked) == 0) {
        free(a->note_path);
        a->note_path = strdup(picked);
        update_window_title(a);
        if (a->vault.dir) vault_scan(&a->vault, a->vault.dir);
        a->vault.selected = vault_index_of(&a->vault, picked);
        fprintf(stderr, "saved (as) %s\n", picked);
    } else {
        fprintf(stderr, "save-as failed: %s\n", picked);
    }
}
static void action_new_file      (App* a) {
    /* Routes through the template picker; if no templates exist, tpl_open
     * does the plain new-file behavior itself. */
    tpl_open(a);
}
static void action_rename        (App* a) {
    if (!a->note_path || strcmp(a->note_path, "(unsaved)") == 0) {
        action_save_as(a); return;
    }
    /* In-app text modal: prefill with the current basename, resolve the
     * typed name relative to the file's parent dir (or use as absolute). */
    {
        char rdir[1024];
        dirname_of(a->note_path, rdir, sizeof rdir);
        if (!app_text_modal(a, "Rename to",
                            vault_basename(a->note_path), rdir)) return;
    }
    if (a->tinput_len == 0) return;
    /* Build picked path: dir reflects any subdir navigation in the modal;
     * use absolute path as-is if typed. */
    char picked[1024];
    bool is_abs = (a->tinput_text[0] == '/' || a->tinput_text[0] == '\\' ||
                   (a->tinput_len >= 2 && a->tinput_text[1] == ':'));
    if (is_abs) {
        snprintf(picked, sizeof picked, "%s", a->tinput_text);
    } else if (a->tinput_dir[0]) {
        snprintf(picked, sizeof picked, "%s/%s",
                 a->tinput_dir, a->tinput_text);
    } else {
        snprintf(picked, sizeof picked, "%s", a->tinput_text);
    }
    if (strcmp(picked, a->note_path) == 0) return;
    /* Snapshot basenames before the move so we can update backlinks. */
    char old_base[256], new_base[256];
    basename_no_md(a->note_path, old_base, sizeof old_base);
    basename_no_md(picked,        new_base, sizeof new_base);
    if (buffer_save(&a->buf, picked) == 0) {
        remove(a->note_path);
        free(a->note_path);
        a->note_path = strdup(picked);
        update_window_title(a);
        if (a->vault.dir) vault_scan(&a->vault, a->vault.dir);
        a->vault.selected = vault_index_of(&a->vault, picked);
        int touched = update_backlinks_in_vault(a, old_base, new_base);
        char msg[160];
        if (touched > 0)
            snprintf(msg, sizeof msg,
                "renamed: updated [[%.40s]] -> [[%.40s]] in %d file(s)",
                old_base, new_base, touched);
        else
            snprintf(msg, sizeof msg, "renamed (no backlinks to update)");
        app_notify(a, msg);
    }
}
static void action_open_file     (App* a) {
    /* Native file picker so the user can open ANY .md on disk, not just
     * notes inside the current vault. Quick switch covers in-vault
     * fuzzy navigation; this complements it. */
    if (!confirm_discard(a)) return;
    char* p = vault_open_dialog(a->window);
    if (!p) return;
    load_note(a, p);
    free(p);
}
static void action_quick_switch  (App* a) { switcher_open(a); }
static void persist_vault_path   (App* a);
static void action_open_dir      (App* a) {
    /* Pick a new vault root via the in-app folder picker. Re-scans the
     * sidebar and persists the choice into settings.lua so the next
     * launch picks it up. */
    char dir[1024];
    const char* start = (a->vault.dir && a->vault.dir[0])
                        ? a->vault.dir : NULL;
    if (!app_dir_modal(a, "Choose vault folder", start, dir, sizeof dir))
        return;
    int n = vault_scan(&a->vault, dir);
    recent_dirs_push(a, dir);
    char msg[300];
    snprintf(msg, sizeof msg, "vault: %s (%d note%s)",
             dir, n, n == 1 ? "" : "s");
    app_notify(a, msg);
    persist_vault_path(a);
}
static void action_command_palette(App* a) { cmdp_open(a); }
static void action_plugins        (App* a) { plugins_open(a); }
static void action_find          (App* a) {
    a->wc_active = false;
    a->search_mode = 1;
    a->search_focus_replace = false;
    if (!SDL_IsTextInputActive()) SDL_StartTextInput();
    app_notify(a,
        "Find: type to search  -  Enter next  -  Shift+Enter prev  -  "
        "Ctrl+H replace  -  Esc close");
}
static void action_find_replace  (App* a) {
    a->wc_active = false;
    a->search_mode = 2;
    a->search_focus_replace = false;
    if (!SDL_IsTextInputActive()) SDL_StartTextInput();
    app_notify(a,
        "Find & Replace: Enter searches  -  "
        "Tab toggles fields  -  Ctrl+Enter replaces all  -  Esc close");
}
static void action_toggle_sidebar(App* a) { a->sidebar_open = !a->sidebar_open; }
static void action_toggle_edit   (App* a) {
    if (a->viewing_image) {
        app_notify(a, "image files are view-only");
        return;
    }
    if (a->edit_mode) enter_preview_mode(a);
    else              enter_edit_mode(a);
}
static void action_undo          (App* a) {
    if (!a->edit_mode) return;
    buffer_undo(&a->buf);
    update_window_title(a);
    ensure_cursor_visible(a);
}
static void action_redo          (App* a) {
    if (!a->edit_mode) return;
    buffer_redo(&a->buf);
    update_window_title(a);
    ensure_cursor_visible(a);
}
static void action_select_all(App* a) { if (a->edit_mode) buffer_select_all(&a->buf); }
static void action_copy      (App* a) {
    if (a->edit_mode) edit_copy(a);
    else              preview_copy(a);
}
static void action_cut       (App* a) {
    if (!a->edit_mode) return;
    edit_cut(a); update_window_title(a);
}
static void action_paste     (App* a) {
    if (!a->edit_mode) return;
    edit_paste(a); update_window_title(a); ensure_cursor_visible(a);
}
static void action_follow_link(App* a) {
    if (!a->edit_mode) return;
    char name[256];
    if (edit_wiki_link_at(&a->buf, a->buf.cursor, name, sizeof name) == 0)
        return;
    follow_wiki_target(a, name);
}
static void action_settings  (App* a) { settings_open(a); }
static void action_help      (App* a) { help_open(a); }
static void action_keybindings(App* a) { keybind_open(a); }
static void action_colors    (App* a) { picker_open(a);  }
static void action_about     (App* a) {
    info_modal(a, "About Downsee " DOWNSEE_VERSION,
        "A keyboard-driven markdown editor.\n"
        "Made by fezcode <samil.bulbul@gmail.com>.\n\n"
        "Stack: C11 + SDL2 + FreeType / HarfBuzz fonts,\n"
        "md4c CommonMark parser, Lua 5.4 plugins,\n"
        "nanosvg icon rasterizer, custom SDF pill AA.");
}
static void action_vsearch   (App* a) { vsearch_open(a); }
static void action_outline   (App* a) { outline_open(a); }
/* action_outline_pin is forward-declared near render_chrome and defined in
 * the outline pinned-panel section. */
static void action_backlinks (App* a) { backlinks_open(a); }
static void action_tags      (App* a) { tags_open(a); }

/* ----------------------------- table align ------------------------------ */
/* Auto-align the markdown table at the cursor. Only basic GFM tables: rows
 * start with `|`, cells separated by `|`, optional `|` at the end. Detects
 * the `|---|---|` alignment row and preserves left/right/center markers. */

#define PREVIEW_TABLE_MAX_COLS 32
#define TABLE_MAX_ROWS 256

static int line_is_table_row(const char* s, size_t n)
{
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    return i < n && s[i] == '|';
}

/* Trim ASCII spaces/tabs from both ends; returns a malloc'd NUL-terminated
 * copy of the trimmed slice. */
static char* trimmed_dup(const char* s, size_t n)
{
    while (n > 0 && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) n--;
    char* r = malloc(n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

/* Split a table row into cells. Returns the count (0..max_cells). Caller
 * frees each cells[i] via free(). Drops a trailing empty cell from `... |`. */
static int parse_table_row(const char* s, size_t n,
                           char** cells, int max_cells)
{
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= n || s[i] != '|') return 0;
    i++;
    int count = 0;
    while (i < n && count < max_cells) {
        size_t cs = i;
        while (i < n && s[i] != '|') i++;
        size_t ce = i;
        cells[count++] = trimmed_dup(s + cs, ce - cs);
        if (i < n) i++;
    }
    if (count > 0 && cells[count - 1][0] == 0) {
        free(cells[count - 1]);
        count--;
    }
    return count;
}

/* Returns 0 = none/left, 1 = left explicit, 2 = right, 3 = center,
 * -1 = not an alignment cell. */
static int cell_alignment_kind(const char* s)
{
    size_t n = strlen(s);
    if (n == 0) return -1;
    int lc = (s[0] == ':');
    int rc = (s[n - 1] == ':');
    size_t lo = lc ? 1 : 0;
    size_t hi = rc ? n - 1 : n;
    if (hi <= lo) return -1;
    for (size_t i = lo; i < hi; ++i) if (s[i] != '-') return -1;
    if (lc && rc) return 3;
    if (rc)       return 2;
    if (lc)       return 1;
    return 0;
}

/* Pad `cell` to `width` chars according to alignment; writes into out. */
static void format_cell(const char* cell, int width, int align,
                        char* out, size_t cap)
{
    int n = (int)strlen(cell);
    if (n > width) n = width;
    int pad_total = width - n;
    int left_pad = 0, right_pad = pad_total;
    if (align == 2)      { left_pad = pad_total; right_pad = 0; }
    else if (align == 3) { left_pad = pad_total / 2; right_pad = pad_total - left_pad; }
    snprintf(out, cap, "%*s%.*s%*s",
             left_pad, "", n, cell, right_pad, "");
}

/* Rewrite the alignment row's cells with the right number of dashes and
 * preserve any `:` markers from the original. */
static void format_align_cell(int width, int kind, char* out, size_t cap)
{
    if (width < 3) width = 3;
    if (cap < (size_t)width + 1) width = (int)cap - 1;
    int pos = 0;
    if (kind == 1 || kind == 3) out[pos++] = ':';
    int dashes = width - pos - ((kind == 2 || kind == 3) ? 1 : 0);
    while (dashes-- > 0 && pos < width) out[pos++] = '-';
    if (kind == 2 || kind == 3) out[pos++] = ':';
    while (pos < width) out[pos++] = '-';
    out[pos] = 0;
}

static void action_align_table(App* a)
{
    if (!a->edit_mode) {
        app_notify(a, "switch to edit mode first");
        return;
    }
    Buffer* b = &a->buf;
    size_t line, col;
    buffer_cursor_pos(b, &line, &col);
    size_t cur_ls = buffer_line_start(b, line);
    size_t cur_le = buffer_line_end(b, line);
    if (!line_is_table_row(b->data + cur_ls, cur_le - cur_ls)) {
        app_notify(a, "not on a table row");
        return;
    }

    /* Walk up + down to find the contiguous table block. */
    size_t first = line;
    while (first > 0) {
        size_t ls = buffer_line_start(b, first - 1);
        size_t le = buffer_line_end(b, first - 1);
        if (!line_is_table_row(b->data + ls, le - ls)) break;
        first--;
    }
    size_t last = line;
    size_t nlines = buffer_line_count(b);
    while (last + 1 < nlines) {
        size_t ls = buffer_line_start(b, last + 1);
        size_t le = buffer_line_end(b, last + 1);
        if (!line_is_table_row(b->data + ls, le - ls)) break;
        last++;
    }

    int n_rows = (int)(last - first + 1);
    if (n_rows > TABLE_MAX_ROWS) {
        app_notify(a, "table too large to align");
        return;
    }

    /* Parse all rows. */
    char* cells[TABLE_MAX_ROWS][PREVIEW_TABLE_MAX_COLS];
    int   ccount[TABLE_MAX_ROWS];
    int   widths[PREVIEW_TABLE_MAX_COLS] = {0};
    int   aligns[PREVIEW_TABLE_MAX_COLS] = {0};
    int   align_row_idx = -1;

    for (int r = 0; r < n_rows; ++r) {
        size_t li = first + (size_t)r;
        size_t ls = buffer_line_start(b, li);
        size_t le = buffer_line_end(b, li);
        ccount[r] = parse_table_row(b->data + ls, le - ls,
                                    cells[r], PREVIEW_TABLE_MAX_COLS);
        /* Detect alignment row (every cell looks like dashes/colons). */
        if (align_row_idx < 0 && ccount[r] > 0) {
            int all_align = 1;
            for (int c = 0; c < ccount[r]; ++c) {
                if (cell_alignment_kind(cells[r][c]) < 0) { all_align = 0; break; }
            }
            if (all_align) {
                align_row_idx = r;
                for (int c = 0; c < ccount[r]; ++c)
                    aligns[c] = cell_alignment_kind(cells[r][c]);
            }
        }
    }
    int n_cols = 0;
    for (int r = 0; r < n_rows; ++r)
        if (ccount[r] > n_cols) n_cols = ccount[r];

    /* Compute max content width per column (skip alignment row's own widths). */
    for (int r = 0; r < n_rows; ++r) {
        if (r == align_row_idx) continue;
        for (int c = 0; c < ccount[r]; ++c) {
            int w = (int)strlen(cells[r][c]);
            if (w > widths[c]) widths[c] = w;
        }
    }
    for (int c = 0; c < n_cols; ++c) if (widths[c] < 3) widths[c] = 3;

    /* Build the rewritten block into a growing buffer. */
    size_t cap = 1024;
    size_t len = 0;
    char*  out = malloc(cap);
    out[0] = 0;
    #define OUT_APPEND(s_, n_) do {                                  \
        if (len + (n_) + 1 > cap) {                                 \
            while (len + (n_) + 1 > cap) cap *= 2;                  \
            out = realloc(out, cap);                                \
        }                                                            \
        memcpy(out + len, s_, n_);                                  \
        len += (n_);                                                 \
        out[len] = 0;                                                \
    } while (0)

    char cell_buf[160];
    for (int r = 0; r < n_rows; ++r) {
        OUT_APPEND("|", 1);
        for (int c = 0; c < n_cols; ++c) {
            OUT_APPEND(" ", 1);
            const char* src = (c < ccount[r]) ? cells[r][c] : "";
            if (r == align_row_idx) {
                format_align_cell(widths[c], aligns[c], cell_buf, sizeof cell_buf);
            } else {
                format_cell(src, widths[c], aligns[c], cell_buf, sizeof cell_buf);
            }
            OUT_APPEND(cell_buf, strlen(cell_buf));
            OUT_APPEND(" |", 2);
        }
        if (r + 1 < n_rows) OUT_APPEND("\n", 1);
    }
    #undef OUT_APPEND

    /* Free the parsed cells. */
    for (int r = 0; r < n_rows; ++r)
        for (int c = 0; c < ccount[r]; ++c) free(cells[r][c]);

    /* Replace the table block in the buffer. */
    size_t replace_start = buffer_line_start(b, first);
    size_t replace_end   = buffer_line_end  (b, last);
    buffer_undo_break(b);
    b->sel_anchor = (long)replace_start;
    b->cursor     = replace_end;
    buffer_insert(b, out, len);
    buffer_undo_break(b);
    free(out);
    update_window_title(a);

    char msg[64];
    snprintf(msg, sizeof msg, "aligned table (%d rows, %d cols)", n_rows, n_cols);
    app_notify(a, msg);
}

/* HTML export uses md4c-html, which streams chunks to a callback. We pipe
 * each chunk into a FILE* opened next to the source note. */
static void html_chunk_cb(const char* chunk, unsigned int n, void* ud)
{
    FILE* f = (FILE*)ud;
    fwrite(chunk, 1, n, f);
}

/* Render the current buffer to HTML next to the source: `note.md` →
 * `note.html`. Wraps the body in a minimal <html><body> shell so the
 * file opens cleanly in a browser. Notifies the user with the result. */
static void action_export_html(App* a)
{
    if (!a->note_path || !*a->note_path ||
        strcmp(a->note_path, "(unsaved)") == 0 ||
        strcmp(a->note_path, "(welcome)") == 0)
    {
        app_notify(a, "save the note first, then export");
        return;
    }
    char out[700];
    snprintf(out, sizeof out, "%s", a->note_path);
    size_t n = strlen(out);
    if (n > 3 && strcmp(out + n - 3, ".md") == 0) {
        out[n - 3] = 0;     /* drop ".md", we'll append ".html" */
    }
    size_t outn = strlen(out);
    if (outn + 6 < sizeof out) snprintf(out + outn, sizeof out - outn, ".html");

    FILE* f = fopen(out, "wb");
    if (!f) {
        char msg[800];
        snprintf(msg, sizeof msg, "export failed: cannot write %s", out);
        app_notify(a, msg);
        return;
    }
    const char* title = vault_basename(a->note_path);
    fprintf(f,
        "<!doctype html>\n<html><head>\n"
        "<meta charset=\"utf-8\">\n"
        "<title>%s</title>\n"
        "<style>body{font-family:Georgia,serif;max-width:780px;"
        "margin:2em auto;padding:0 1em;line-height:1.5;}"
        "code,pre{font-family:Consolas,monospace;background:#f3f3f3;}"
        "pre{padding:.5em;overflow:auto;}"
        "blockquote{border-left:4px solid #ccc;padding-left:.8em;color:#555;}"
        "</style>\n</head>\n<body>\n",
        title);

    int rc = md_html(a->buf.data, (unsigned)a->buf.len,
                     html_chunk_cb, f,
                     /* parser flags */ MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH |
                                        MD_FLAG_TASKLISTS,
                     /* render flags */ 0);
    fprintf(f, "\n</body>\n</html>\n");
    fclose(f);

    char msg[800];
    if (rc == 0) snprintf(msg, sizeof msg, "exported \xe2\x86\x92 %s", out);
    else         snprintf(msg, sizeof msg, "export failed (md4c err %d)", rc);
    app_notify(a, msg);
}

/* Open today's daily note. Path is `<vault>/daily/YYYY-MM-DD.md`; the
 * `daily` folder is created on demand and a header stub is written if the
 * file doesn't exist yet. Falls back to vault root if the daily/ mkdir
 * fails (so the action always lands somewhere sensible). */
static void action_daily(App* a)
{
    if (!confirm_discard(a)) return;
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    char date[16];
    if (!tm || strftime(date, sizeof date, "%Y-%m-%d", tm) == 0) {
        snprintf(date, sizeof date, "today");
    }
    const char* root = (a->vault.dir && *a->vault.dir) ? a->vault.dir : "data";
    char dir[512], path[600];
    snprintf(dir,  sizeof dir,  "%s/daily", root);
    snprintf(path, sizeof path, "%s/%s.md", dir, date);
#ifdef _WIN32
    CreateDirectoryA(dir, NULL);     /* harmless if already present */
#else
    mkdir(dir, 0755);
#endif

    FILE* exists = fopen(path, "rb");
    if (!exists) {
        FILE* nf = fopen(path, "wb");
        if (nf) {
            fprintf(nf, "# %s\n\n", date);
            fclose(nf);
        }
    } else {
        fclose(exists);
    }
    if (a->vault.dir) vault_scan(&a->vault, a->vault.dir);
    if (load_note(a, path) == 0) {
        char msg[160];
        snprintf(msg, sizeof msg, "daily note: %s", date);
        app_notify(a, msg);
    }
}

static const ActionEntry ACTIONS[] = {
    { "new_file",        "File",       action_new_file        },
    { "open_file",       "File",       action_open_file       },
    { "open_dir",        "File",       action_open_dir        },
    { "quick_switch",    "File",       action_quick_switch    },
    { "command_palette", "App",        action_command_palette },
    { "plugins",         "App",        action_plugins         },
    { "save",            "File",       action_save            },
    { "save_as",         "File",       action_save_as         },
    { "rename",          "File",       action_rename          },
    { "quit",            "File",       action_quit            },

    { "undo",            "Edit",       action_undo            },
    { "redo",            "Edit",       action_redo            },
    { "select_all",      "Edit",       action_select_all      },
    { "cut",             "Edit",       action_cut             },
    { "copy",            "Edit",       action_copy            },
    { "paste",           "Edit",       action_paste           },
    { "find",            "Edit",       action_find            },
    { "find_replace",    "Edit",       action_find_replace    },
    { "vault_search",    "Edit",       action_vsearch         },

    { "follow_link",     "Navigation", action_follow_link     },
    { "outline",         "Navigation", action_outline         },
    { "outline_pin",     "Navigation", action_outline_pin     },
    { "backlinks",       "Navigation", action_backlinks       },
    { "tags",            "Navigation", action_tags            },
    { "daily_note",      "Navigation", action_daily           },
    { "export_html",     "File",       action_export_html     },
    { "align_table",     "Edit",       action_align_table     },

    { "toggle_edit",     "View",       action_toggle_edit     },
    { "toggle_sidebar",  "View",       action_toggle_sidebar  },

    { "settings",        "App",        action_settings        },
    { "keybindings",     "App",        action_keybindings     },
    { "colors",          "App",        action_colors          },
    { "help",            "App",        action_help            },
    { NULL, NULL, NULL },
};

static const struct { const char* keystr; const char* action; } DEFAULT_KEYS[] = {
    { "ctrl+q",       "quit"           },
    { "ctrl+s",       "save"           },
    { "ctrl+shift+s", "save_as"        },
    { "ctrl+n",       "new_file"       },
    { "f2",           "rename"         },
    { "ctrl+o",       "open_file"      },
    { "ctrl+p",       "quick_switch"   },
    { "ctrl+shift+p", "command_palette"},
    { "ctrl+alt+p",   "plugins"        },
    { "ctrl+f",       "find"           },
    { "ctrl+h",       "find_replace"   },
    { "ctrl+shift+f", "vault_search"   },
    { "ctrl+shift+o", "outline"        },
    { "ctrl+alt+o",   "outline_pin"    },
    { "ctrl+shift+b", "backlinks"      },
    { "ctrl+shift+g", "tags"           },
    { "ctrl+d",       "daily_note"     },
    { "ctrl+shift+e", "export_html"    },
    { "ctrl+alt+t",   "align_table"    },
    { "ctrl+b",       "toggle_sidebar" },
    { "ctrl+e",       "toggle_edit"    },
    { "ctrl+z",       "undo"           },
    { "ctrl+shift+z", "redo"           },
    { "ctrl+y",       "redo"           },
    { "ctrl+a",       "select_all"     },
    { "ctrl+c",       "copy"           },
    { "ctrl+x",       "cut"            },
    { "ctrl+v",       "paste"          },
    { "f12",          "follow_link"    },
    { "ctrl+return",  "follow_link"    },
    { "ctrl+,",       "settings"       },
    { "f10",          "settings"       },
    { "f1",           "help"           },
    { "ctrl+k",       "keybindings"    },
    { "ctrl+shift+t", "colors"         },
    { NULL, NULL },
};

static ActionFn find_action(const char* name)
{
    if (!name) return NULL;
    for (int i = 0; ACTIONS[i].name; ++i)
        if (strcmp(ACTIONS[i].name, name) == 0) return ACTIONS[i].fn;
    return NULL;
}

static const char* default_action_for(const char* keystr)
{
    for (int i = 0; DEFAULT_KEYS[i].keystr; ++i)
        if (strcmp(DEFAULT_KEYS[i].keystr, keystr) == 0)
            return DEFAULT_KEYS[i].action;
    return NULL;
}

/* Reverse lookup for the keybindings overlay: find the first DEFAULT_KEYS
 * entry whose action matches `action` AND whose keystr hasn't been
 * re-bound to a different action by the user. Returns "" if none. */
static const char* default_keystr_for_action(const char* action)
{
    for (int i = 0; DEFAULT_KEYS[i].keystr; ++i) {
        if (strcmp(DEFAULT_KEYS[i].action, action) != 0) continue;
        const char* shadow = user_kbind_for(DEFAULT_KEYS[i].keystr);
        if (shadow && strcmp(shadow, action) != 0) continue;
        return DEFAULT_KEYS[i].keystr;
    }
    return "";
}

static int ACTIONS_count(void)
{
    int n = 0;
    while (ACTIONS[n].name) n++;
    return n;
}

static const char* ACTIONS_name(int i)
{
    if (i < 0) return "";
    int n = 0;
    while (ACTIONS[n].name) {
        if (n == i) return ACTIONS[n].name;
        n++;
    }
    return "";
}

static const char* ACTIONS_category(int i)
{
    if (i < 0) return "";
    int n = 0;
    while (ACTIONS[n].name) {
        if (n == i) return ACTIONS[n].category;
        n++;
    }
    return "";
}

/* True when every DEFAULT_KEYS keystr that maps to this action has been
 * re-bound by the user to a different action — leaving the action with no
 * default fallback. Used to flag silently-shadowed actions in the overlay. */
static int action_is_shadowed(const char* action)
{
    int has_default = 0;
    int all_taken   = 1;
    for (int i = 0; DEFAULT_KEYS[i].keystr; ++i) {
        if (strcmp(DEFAULT_KEYS[i].action, action) != 0) continue;
        has_default = 1;
        const char* shadow = user_kbind_for(DEFAULT_KEYS[i].keystr);
        if (!shadow || strcmp(shadow, action) == 0) {
            all_taken = 0;
            break;
        }
    }
    return has_default && all_taken;
}

/* When `action` is shadowed, find the action that took its first default key.
 * Used to surface "(taken by X)" so the user understands what happened.
 * Returns NULL if not shadowed or if the shadowing action can't be named. */
static const char* action_shadower(const char* action)
{
    for (int i = 0; DEFAULT_KEYS[i].keystr; ++i) {
        if (strcmp(DEFAULT_KEYS[i].action, action) != 0) continue;
        const char* shadow = user_kbind_for(DEFAULT_KEYS[i].keystr);
        if (shadow && strcmp(shadow, action) != 0) return shadow;
    }
    return NULL;
}

/* "ctrl+shift+s" — modifier order is ctrl, shift, alt. Key name is from
 * SDL_GetKeyName lowercased, spaces converted to underscores. */
static void build_keystr(SDL_Keycode k, int mod, char* out, size_t outlen)
{
    int n = 0;
    if (mod & KMOD_CTRL)  n += snprintf(out + n, outlen - n, "ctrl+");
    if (mod & KMOD_SHIFT) n += snprintf(out + n, outlen - n, "shift+");
    if (mod & KMOD_ALT)   n += snprintf(out + n, outlen - n, "alt+");

    const char* name = SDL_GetKeyName(k);
    if (!name || !*name) { snprintf(out + n, outlen - n, "?"); return; }
    char nb[40];
    snprintf(nb, sizeof nb, "%s", name);
    for (char* p = nb; *p; ++p) {
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
        else if (*p == ' ')         *p = '_';
    }
    snprintf(out + n, outlen - n, "%s", nb);
}

/* ----------------------------- event loop ------------------------------- */

static void app_event(App* a, const SDL_Event* e)
{
    int line_px   = font_line_height(a->font_body);
    int page_step = viewport_h(a) - line_px;
    if (page_step < line_px) page_step = line_px;

    switch (e->type) {
        case SDL_QUIT:
            if (confirm_discard(a)) a->running = false;
            break;

        case SDL_WINDOWEVENT:
            if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                a->win_w = e->window.data1;
                a->win_h = e->window.data2;
                clamp_scroll(a);
                /* Flash a centered "WxH" badge so the user has feedback
                 * while dragging the window edge. Hides ~900ms after the
                 * last resize event. */
                a->resize_show_until = SDL_GetTicks() + 900;
                a->wants_anim_frame  = true;
            }
            break;

        case SDL_MOUSEMOTION: {
            a->chrome_hover  = chrome_hit_test(a, e->motion.x, e->motion.y);
            a->tb_btn_hover  = titlebar_button_at(a, e->motion.x, e->motion.y);
            a->menu_hover    = titlebar_menu_at  (a, e->motion.x, e->motion.y);
            a->sidebar_hover = sidebar_item_at(a, e->motion.x, e->motion.y);
            if (a->switcher_active) {
                int row = switcher_row_at(a, e->motion.x, e->motion.y);
                if (row >= 0) a->switcher_selected = row;
            }
            if (a->cmdp_active) {
                /* Hover is its own state — never moves cmdp_selected on
                 * mouse motion, so the list doesn't auto-pan when you
                 * move the cursor. Keyboard (arrows) and wheel move scroll;
                 * click invokes the hovered row. */
                a->cmdp_hover = cmdp_row_at(a, e->motion.x, e->motion.y);
                /* Drag thumb if the user grabbed it. */
                if (a->sb_drag == SB_CMDP) {
                    SDL_Rect track, thumb;
                    cmdp_geom(a, NULL, NULL, &track, &thumb);
                    if (track.w > 0) {
                        SDL_Rect inner; sb_inner_track(&track, &inner);
                        int max_sc = cmdp_max_scroll(a);
                        a->cmdp_scroll = scroll_from_thumb_drag(
                            e->motion.y, inner.y, inner.h, thumb.h,
                            a->sb_drag_offset, max_sc);
                    }
                }
            }
            /* Resize-edge cursor: ask the hit-test where this point lives,
             * pick a cursor, only call SDL_SetCursor when it changes. */
            {
                SDL_Point p = { e->motion.x, e->motion.y };
                SDL_HitTestResult ht =
                    window_hit_test_cb(a->window, &p, a);
                int kind = 0;       /* arrow */
                SDL_Cursor* cur = a->cur_arrow;
                switch (ht) {
                    case SDL_HITTEST_RESIZE_TOP:
                    case SDL_HITTEST_RESIZE_BOTTOM:
                        kind = 1; cur = a->cur_ns;   break;
                    case SDL_HITTEST_RESIZE_LEFT:
                    case SDL_HITTEST_RESIZE_RIGHT:
                        kind = 2; cur = a->cur_we;   break;
                    case SDL_HITTEST_RESIZE_TOPLEFT:
                    case SDL_HITTEST_RESIZE_BOTTOMRIGHT:
                        kind = 3; cur = a->cur_nwse; break;
                    case SDL_HITTEST_RESIZE_TOPRIGHT:
                    case SDL_HITTEST_RESIZE_BOTTOMLEFT:
                        kind = 4; cur = a->cur_nesw; break;
                    default: break;
                }
                if (kind != a->cur_kind && cur) {
                    SDL_SetCursor(cur);
                    a->cur_kind = kind;
                }
            }
            /* Breadcrumb hover hit-test. Rects come from the last frame's
             * render_chrome (close enough; chrome doesn't reflow per-tick). */
            a->crumb_hover = -1;
            int mx = e->motion.x, my = e->motion.y;
            const SDL_Rect* cv = &a->crumb_rect_vault;
            const SDL_Rect* ct = &a->crumb_rect_title;
            if (cv->w > 0 && mx >= cv->x && mx < cv->x + cv->w &&
                my >= cv->y && my < cv->y + cv->h)        a->crumb_hover = 0;
            else if (ct->w > 0 && mx >= ct->x && mx < ct->x + ct->w &&
                     my >= ct->y && my < ct->y + ct->h)   a->crumb_hover = 1;
            if (a->ctx_menu_active) {
                int parent_row = ctx_menu_row_at(a, e->motion.x, e->motion.y);
                int sub_row    = a->ctx_submenu_active
                    ? submenu_row_at(a, e->motion.x, e->motion.y)
                    : -1;
                /* Don't change parent hover when the cursor is inside the
                 * submenu — keeps the recent-vaults row highlighted while
                 * the user picks an entry. */
                if (sub_row < 0) a->ctx_menu_hover = parent_row;
                a->ctx_submenu_hover = sub_row;
                /* Open the submenu the moment the cursor lands on the
                 * recent-vaults row. Close it when the cursor moves to a
                 * different parent row that isn't the submenu itself. */
                if (parent_row >= 0 &&
                    ctx_is_recent_submenu_row(a, parent_row))
                {
                    if (!a->ctx_submenu_active) {
                        a->ctx_submenu_active = true;
                        a->ctx_submenu_hover  = -1;
                        a->ctx_submenu_open_t = 0.0f;
                        for (int r = 0; r < 16; ++r)
                            a->ctx_submenu_row_t[r] = 0.0f;
                    }
                } else if (parent_row >= 0 && sub_row < 0 &&
                           a->ctx_submenu_active)
                {
                    a->ctx_submenu_active = false;
                    a->ctx_submenu_hover  = -1;
                }
            }
            if (a->settings_active)
                a->settings_hover =
                    settings_hit_test(a, e->motion.x, e->motion.y, NULL, NULL);
            if (a->keybind_active)
                a->keybind_hover =
                    keybind_hit_test(a, e->motion.x, e->motion.y);
            if (a->picker_active)
                a->picker_hover =
                    picker_hit_test(a, e->motion.x, e->motion.y, NULL, NULL);
            if (a->vsearch_active)
                a->vsearch_hover =
                    vsearch_hit_test(a, e->motion.x, e->motion.y);
            if (a->outline_active)
                a->outline_hover =
                    outline_hit_test(a, e->motion.x, e->motion.y);
            else if (a->outline_pinned)
                a->outline_hover =
                    outline_panel_hit_test(a, e->motion.x, e->motion.y);
            else
                a->outline_hover = -1;
            if (a->backlinks_active)
                a->backlinks_hover =
                    blink_hit_test(a, e->motion.x, e->motion.y);
            if (a->tags_active)
                a->tags_hover =
                    tags_hit_test(a, e->motion.x, e->motion.y);
            if (a->tpl_active)
                a->tpl_hover =
                    tpl_hit_test(a, e->motion.x, e->motion.y);

            /* Link tooltip in preview: walk the per-frame hit rects for
             * the one under the cursor. Wiki targets resolve to a vault
             * file or "no match"; inline links show their href. Cleared
             * each motion so the tip disappears the instant the cursor
             * leaves a link. */
            a->tip_active = false;
            if (!a->edit_mode && !a->switcher_active && !a->cmdp_active &&
                !a->plugins_active && !a->settings_active &&
                !a->keybind_active && !a->picker_active &&
                !a->ctx_menu_active)
            {
                for (size_t hi = 0; hi < a->hit_count; ++hi) {
                    struct ClickHit* h = &a->hits[hi];
                    if (e->motion.x < h->rect.x ||
                        e->motion.x >= h->rect.x + h->rect.w ||
                        e->motion.y < h->rect.y ||
                        e->motion.y >= h->rect.y + h->rect.h) continue;
                    if (h->kind != HIT_WIKI) continue;
                    /* Inline `[text](url)` ranges win over wiki resolution
                     * since they may overlap byte-wise. */
                    bool resolved = false;
                    for (size_t li = 0; li < a->doc.link_count; ++li) {
                        MdLink* lk = &a->doc.links[li];
                        if (h->byte_start < lk->start ||
                            h->byte_start >= lk->end) continue;
                        snprintf(a->tip_text, sizeof a->tip_text,
                                 "Open external -- %.220s",
                                 lk->href ? lk->href : "");
                        a->tip_broken = false;
                        resolved = true;
                        break;
                    }
                    if (!resolved) {
                        for (size_t wi = 0; wi < a->doc.wiki_count; ++wi) {
                            MdWiki* wk = &a->doc.wikis[wi];
                            if (h->byte_start < wk->start ||
                                h->byte_start >= wk->end) continue;
                            char target[256];
                            size_t n = wk->name_len < sizeof target - 1
                                        ? wk->name_len : sizeof target - 1;
                            memcpy(target, a->doc.data + wk->name_start, n);
                            target[n] = 0;
                            const char* found = NULL;
                            for (size_t v = 0; v < a->vault.count; ++v) {
                                if (a->vault.items[v].is_dir) continue;
                                char base[256];
                                snprintf(base, sizeof base, "%s",
                                         a->vault.items[v].name);
                                size_t bl = strlen(base);
                                if (bl > 3 && strieq(base + bl - 3, ".md"))
                                    base[bl - 3] = 0;
                                if (strieq(base, target)) {
                                    found = a->vault.items[v].path;
                                    break;
                                }
                            }
                            if (found) {
                                snprintf(a->tip_text, sizeof a->tip_text,
                                         "Open -- %.220s", found);
                                a->tip_broken = false;
                            } else {
                                snprintf(a->tip_text, sizeof a->tip_text,
                                         "[[%.180s]] -- no matching note",
                                         target);
                                a->tip_broken = true;
                            }
                            resolved = true;
                            break;
                        }
                    }
                    if (resolved) {
                        a->tip_active   = true;
                        a->tip_anchor_x = e->motion.x;
                        a->tip_anchor_y = e->motion.y;
                    }
                    break;     /* first hit wins */
                }
            }

            /* Cursor change when hovering EITHER the sidebar resize handle
             * or the outline-panel resize handle (left edge of pinned panel). */
            {
                bool over_resize = false;
                if (a->sidebar_open &&
                    abs(e->motion.x - a->sidebar_w) <= 3)
                    over_resize = true;
                if (a->outline_pinned) {
                    int px = a->win_w - a->outline_panel_w;
                    if (abs(e->motion.x - px) <= 3) over_resize = true;
                }
                if (over_resize) {
                    if (!a->cursor_is_resize && a->cursor_resize) {
                        SDL_SetCursor(a->cursor_resize);
                        a->cursor_is_resize = true;
                    }
                } else if (a->cursor_is_resize) {
                    SDL_SetCursor(SDL_GetDefaultCursor());
                    a->cursor_is_resize = false;
                }
            }

            /* Active sidebar resize: track mouse x. */
            if (a->resizing_sidebar &&
                (e->motion.state & SDL_BUTTON_LMASK)) {
                int w = e->motion.x;
                if (w < 120)              w = 120;
                if (w > a->win_w / 2)     w = a->win_w / 2;
                a->sidebar_w = w;
                clamp_scroll(a);
                break;
            }
            /* Active outline-panel resize: width = win_w - mouse.x. */
            if (a->resizing_outline &&
                (e->motion.state & SDL_BUTTON_LMASK)) {
                int w = a->win_w - e->motion.x;
                if (w < 160)              w = 160;
                if (w > a->win_w / 2)     w = a->win_w / 2;
                a->outline_panel_w = w;
                clamp_scroll(a);
                break;
            }
            /* Active scrollbar drag: translate mouse y to scroll position. */
            if (a->sb_drag != SB_NONE &&
                (e->motion.state & SDL_BUTTON_LMASK)) {
                SDL_Rect track, thumb;
                int  content_h = 0;
                int* scroll    = NULL;
                bool got       = false;
                switch (a->sb_drag) {
                case SB_DOC:
                    if (doc_scrollbar_geom(a, &track, &thumb)) {
                        scroll = &a->scroll_y;
                        content_h = a->doc_height_px;
                        got = true;
                    }
                    break;
                case SB_KEYBIND:
                    if (keybind_scrollbar_geom(a, &track, &thumb)) {
                        scroll = &a->keybind_scroll;
                        content_h = g_kbind_content_h;
                        got = true;
                    }
                    break;
                case SB_VSEARCH:
                    if (vsearch_scrollbar_geom(a, &track, &thumb)) {
                        scroll = &a->vsearch_scroll;
                        content_h = vsearch_row_h(a) * a->vsearch_count;
                        got = true;
                    }
                    break;
                case SB_OUTLINE_LIST:
                    if (outline_scrollbar_geom(a, &track, &thumb)) {
                        scroll = &a->outline_scroll;
                        content_h = outline_row_h(a) * a->outline_count;
                        got = true;
                    }
                    break;
                case SB_BACKLINKS:
                    if (backlinks_scrollbar_geom(a, &track, &thumb)) {
                        scroll = &a->backlinks_scroll;
                        content_h = blink_row_h(a) * a->backlinks_count;
                        got = true;
                    }
                    break;
                case SB_TAGS:
                    if (tags_scrollbar_geom(a, &track, &thumb)) {
                        scroll = &a->tags_scroll;
                        content_h = tags_row_h(a) * a->tags_count;
                        got = true;
                    }
                    break;
                case SB_PICKER:
                    if (picker_scrollbar_geom(a, &track, &thumb)) {
                        scroll = &a->picker_scroll;
                        content_h = picker_row_h(a) * COLOR_SLOT_COUNT;
                        got = true;
                    }
                    break;
                default:
                    break;
                }
                if (got && scroll) {
                    SDL_Rect inner; sb_inner_track(&track, &inner);
                    int max_sc = content_h - track.h;
                    if (max_sc < 0) max_sc = 0;
                    *scroll = scroll_from_thumb_drag(e->motion.y,
                        inner.y, inner.h, thumb.h,
                        a->sb_drag_offset, max_sc);
                    if (a->sb_drag == SB_DOC) clamp_scroll(a);
                }
                break;
            }

            /* Sidebar DnD: arm → active when motion exceeds threshold; once
             * active, track cursor and update drop target. */
            if ((a->dnd_armed || a->dnd_active) &&
                (e->motion.state & SDL_BUTTON_LMASK)) {
                if (a->dnd_armed && !a->dnd_active) {
                    int dx = e->motion.x - a->dnd_press_x;
                    int dy = e->motion.y - a->dnd_press_y;
                    if (dx*dx + dy*dy >= DND_DRAG_THRESHOLD_PX*DND_DRAG_THRESHOLD_PX) {
                        a->dnd_active = true;
                        a->dnd_armed  = false;
                    }
                }
                if (a->dnd_active) {
                    a->dnd_x = e->motion.x;
                    a->dnd_y = e->motion.y;
                    a->dnd_drop_target =
                        dnd_find_drop_target(a, e->motion.x, e->motion.y);
                    /* Don't allow dropping onto self / own current parent. */
                    if (a->dnd_source_idx >= 0 &&
                        a->dnd_source_idx < (int)a->vault.count)
                    {
                        const VaultItem* src = &a->vault.items[a->dnd_source_idx];
                        if (a->dnd_drop_target >= 0 &&
                            a->dnd_drop_target < (int)a->vault.count &&
                            strcmp(a->vault.items[a->dnd_drop_target].path,
                                   src->path) == 0)
                            a->dnd_drop_target = -2;
                    }
                }
            }

            /* Edit-mode drag-extend. */
            if (a->mouse_selecting && a->edit_mode &&
                (e->motion.state & SDL_BUTTON_LMASK)) {
                size_t pos = edit_position_at(a, e->motion.x, e->motion.y);
                a->buf.cursor = pos;
                if (a->buf.cursor > a->buf.len) a->buf.cursor = a->buf.len;
                bump_blink(a);
            }

            /* Preview-mode drag-extend. */
            if (a->preview_selecting && !a->edit_mode &&
                (e->motion.state & SDL_BUTTON_LMASK)) {
                a->preview_sel_end = preview_position_at(a,
                    e->motion.x, e->motion.y);
            }
            break;
        }

        case SDL_MOUSEBUTTONUP:
            if (e->button.button == SDL_BUTTON_LEFT) {
                /* DnD finalize: if we ended up dragging, do the move; if it
                 * was a click (armed but never crossed threshold), defer to
                 * the loaded behavior — load the file. */
                if (a->dnd_active) {
                    if (a->dnd_drop_target != -2) {
                        if (confirm_discard(a)) dnd_finish_move(a);
                    }
                    dnd_reset(a);
                } else if (a->dnd_armed) {
                    int idx = a->dnd_source_idx;
                    dnd_reset(a);
                    if (idx >= 0 && idx < (int)a->vault.count) {
                        VaultItem* it = &a->vault.items[idx];
                        if (!it->is_dir && confirm_discard(a))
                            load_note(a, it->path);
                    }
                }
                a->mouse_selecting   = false;
                a->preview_selecting = false;
                a->resizing_sidebar  = false;
                a->resizing_outline  = false;
                a->sb_drag           = SB_NONE;
            }
            break;

        case SDL_MOUSEBUTTONDOWN:
            /* Pinned outline panel: click a row to jump cursor there. */
            if (a->outline_pinned && e->button.button == SDL_BUTTON_LEFT) {
                int row = outline_panel_hit_test(a, e->button.x, e->button.y);
                if (row >= 0) {
                    outline_panel_activate(a, row);
                    break;
                }
            }
            /* Title-bar window controls: min / max / close. Always
             * available, even with overlays open. */
            if (e->button.button == SDL_BUTTON_LEFT) {
                int tbb = titlebar_button_at(a, e->button.x, e->button.y);
                if (tbb != TBB_NONE) {
                    titlebar_button_invoke(a, tbb);
                    break;
                }
            }
            /* Quick switcher (Open dialog): click row → open it; click
             * outside the box → dismiss. */
            if (a->switcher_active && e->button.button == SDL_BUTTON_LEFT) {
                int row = switcher_row_at(a, e->button.x, e->button.y);
                if (row >= 0) {
                    a->switcher_selected = row;
                    switcher_select(a);
                } else {
                    /* Outside the box → close. */
                    int box_w = 560;
                    int box_x = (a->win_w - box_w) / 2;
                    int box_y = 90;
                    int max_rows = 12;
                    int row_h    = font_line_height(a->font_body) + 8;
                    int rows     = a->switcher_count < max_rows ? a->switcher_count : max_rows;
                    if (rows == 0) rows = 1;
                    int box_h = row_h * (1 + rows) + 18;
                    if (e->button.x < box_x || e->button.x >= box_x + box_w ||
                        e->button.y < box_y || e->button.y >= box_y + box_h)
                        switcher_close(a);
                }
                break;
            }
            /* Plugins overlay: Reload button + click-outside-to-close. */
            if (a->plugins_active && e->button.button == SDL_BUTTON_LEFT) {
                if (plugins_reload_hit(a, e->button.x, e->button.y)) {
                    plugins_action_reload(a);
                    break;
                }
                SDL_Rect box = plugins_box_rect(a);
                if (e->button.x < box.x || e->button.x >= box.x + box.w ||
                    e->button.y < box.y || e->button.y >= box.y + box.h)
                    plugins_close(a);
                break;
            }
            /* Command palette: scrollbar > row click > click-outside-to-close. */
            if (a->cmdp_active && e->button.button == SDL_BUTTON_LEFT) {
                /* Scrollbar takes priority — arrows step, thumb starts a
                 * drag, inner track jumps. */
                SDL_Rect track, thumb;
                cmdp_geom(a, NULL, NULL, &track, &thumb);
                if (track.w > 0 &&
                    overlay_scrollbar_handle_click(a, e->button.x, e->button.y,
                        &track, &thumb, SB_CMDP, &a->cmdp_scroll,
                        a->cmdp_count * cmdp_row_h(a), cmdp_row_h(a)))
                {
                    cmdp_clamp_scroll(a);
                    break;
                }
                int row = cmdp_row_at(a, e->button.x, e->button.y);
                if (row >= 0) {
                    cmdp_invoke_index(a, row);
                    break;
                }
                SDL_Rect box;
                cmdp_geom(a, &box, NULL, NULL, NULL);
                if (e->button.x < box.x || e->button.x >= box.x + box.w ||
                    e->button.y < box.y || e->button.y >= box.y + box.h)
                    cmdp_close(a);
                break;
            }
            /* Title-bar menu items: open the matching dropdown. Close any
             * other floating overlay first so the dropdown isn't stacked
             * on top of stale state. */
            if (e->button.button == SDL_BUTTON_LEFT && !a->ctx_menu_active) {
                int mi = titlebar_menu_at(a, e->button.x, e->button.y);
                if (mi >= 0) {
                    if (a->backlinks_active) backlinks_close(a);
                    if (a->tags_active)      tags_close(a);
                    if (a->vsearch_active)   vsearch_close(a);
                    if (a->outline_active)   outline_close(a);
                    if (a->tpl_active)       tpl_close(a);
                    if (a->picker_active)    picker_close(a);
                    if (a->keybind_active)   keybind_close(a);
                    if (a->settings_active)  settings_close(a);
                    if (a->help_active)      help_close(a);
                    a->menu_open = mi;
                    SDL_Rect r = a->menu_rects[mi];
                    ctx_menu_open_menu(a, mi, r.x, r.y + r.h + 2);
                    break;
                }
            }
            /* Chrome bar: clicks here only fire if NO overlay is open. The
             * overlay-handlers below intercept first. */
            if (e->button.button == SDL_BUTTON_LEFT &&
                !a->backlinks_active && !a->tags_active && !a->tpl_active &&
                !a->outline_active   && !a->vsearch_active &&
                !a->picker_active    && !a->keybind_active &&
                !a->settings_active  && !a->help_active   &&
                !a->ctx_menu_active)
            {
                int btn = chrome_hit_test(a, e->button.x, e->button.y);
                if (btn != CB_NONE) {
                    a->chrome_press_t[btn] = 1.0f;     /* press flash */
                    chrome_button_invoke(a, btn);
                    break;
                }
                /* Breadcrumb clicks: vault name toggles sidebar; note title
                 * scrolls the document to top. */
                if (a->crumb_hover == 0) {
                    a->sidebar_open = !a->sidebar_open;
                    break;
                }
                if (a->crumb_hover == 1) {
                    a->scroll_y = 0;
                    clamp_scroll(a);
                    break;
                }
            }
            /* Backlinks overlay: click row to open the source file. */
            if (a->backlinks_active && e->button.button == SDL_BUTTON_LEFT) {
                SDL_Rect btrack, bthumb;
                if (backlinks_scrollbar_geom(a, &btrack, &bthumb) &&
                    overlay_scrollbar_handle_click(a, e->button.x, e->button.y,
                        &btrack, &bthumb, SB_BACKLINKS,
                        &a->backlinks_scroll,
                        blink_row_h(a) * a->backlinks_count,
                        blink_row_h(a)))
                    break;
                int row = blink_hit_test(a, e->button.x, e->button.y);
                int rh    = blink_row_h(a);
                int box_w = BLINK_BOX_W;
                int box_x = (a->win_w - box_w) / 2;
                int box_y = BLINK_BOX_Y;
                int max_box_h = a->win_h - 80;
                int box_h = rh * (a->backlinks_count + 3) + 24;
                if (box_h > max_box_h) box_h = max_box_h;
                /* Click outside the box (any side) closes the overlay. */
                if (e->button.x < box_x ||
                    e->button.x >= box_x + box_w ||
                    e->button.y < box_y ||
                    e->button.y >= box_y + box_h)
                {
                    backlinks_close(a);
                    break;
                }
                if (row >= 0) {
                    a->backlinks_selected = row;
                    backlinks_activate(a);
                }
                break;
            }
            /* Tags overlay: click row to run vault search for `#tag`. */
            if (a->tags_active && e->button.button == SDL_BUTTON_LEFT) {
                SDL_Rect ttrack, tthumb;
                if (tags_scrollbar_geom(a, &ttrack, &tthumb) &&
                    overlay_scrollbar_handle_click(a, e->button.x, e->button.y,
                        &ttrack, &tthumb, SB_TAGS,
                        &a->tags_scroll,
                        tags_row_h(a) * a->tags_count,
                        tags_row_h(a)))
                    break;
                int row = tags_hit_test(a, e->button.x, e->button.y);
                int box_w = TAGS_BOX_W;
                int box_x = (a->win_w - box_w) / 2;
                if (e->button.x < box_x ||
                    e->button.x >= box_x + box_w)
                {
                    tags_close(a);
                    break;
                }
                if (row >= 0) {
                    a->tags_selected = row;
                    tags_activate(a);
                }
                break;
            }
            /* Template picker: click to instantiate; click outside = blank file. */
            if (a->tpl_active && e->button.button == SDL_BUTTON_LEFT) {
                int row = tpl_hit_test(a, e->button.x, e->button.y);
                int box_w = TPL_BOX_W;
                int box_x = (a->win_w - box_w) / 2;
                if (e->button.x < box_x ||
                    e->button.x >= box_x + box_w)
                {
                    tpl_close(a);
                    break;
                }
                if (row >= 0) {
                    a->tpl_selected = row;
                    tpl_activate(a);
                }
                break;
            }
            /* Outline overlay: click row to jump. Click outside the box closes. */
            if (a->outline_active && e->button.button == SDL_BUTTON_LEFT) {
                SDL_Rect otrack, othumb;
                if (outline_scrollbar_geom(a, &otrack, &othumb) &&
                    overlay_scrollbar_handle_click(a, e->button.x, e->button.y,
                        &otrack, &othumb, SB_OUTLINE_LIST,
                        &a->outline_scroll,
                        outline_row_h(a) * a->outline_count,
                        outline_row_h(a)))
                    break;
                int row = outline_hit_test(a, e->button.x, e->button.y);
                int box_w = OUTLINE_BOX_W;
                int box_x = (a->win_w - box_w) / 2;
                if (e->button.x < box_x ||
                    e->button.x >= box_x + box_w)
                {
                    outline_close(a);
                    break;
                }
                if (row >= 0) {
                    a->outline_selected = row;
                    outline_activate(a);
                }
                break;
            }
            /* Vault search overlay: click hit row to open it. Click outside
             * the box to close. */
            /* Search bar (Ctrl+F / Ctrl+H): chip toggles, focus switch,
             * click-outside-to-close. Runs BEFORE the vsearch handler
             * because both can be active independently. */
            if (a->search_mode != 0 && e->button.button == SDL_BUTTON_LEFT) {
                int hit = search_overlay_hit_test(a, e->button.x, e->button.y);
                if (hit == SEARCH_HIT_TOGGLE) {
                    a->search_mode = (a->search_mode == 2) ? 1 : 2;
                    if (a->search_mode == 1) a->search_focus_replace = false;
                    break;
                }
                if (hit == SEARCH_HIT_CHIP_RE) {
                    a->search_regex = !a->search_regex;
                    search_rebuild(a);
                    break;
                }
                if (hit == SEARCH_HIT_CHIP_AA) {
                    a->search_case_insensitive = !a->search_case_insensitive;
                    search_rebuild(a);
                    break;
                }
                if (hit == SEARCH_HIT_CHIP_W) {
                    a->search_whole_word = !a->search_whole_word;
                    search_rebuild(a);
                    break;
                }
                if (hit == SEARCH_HIT_BTN_REPL) {
                    search_replace_one(a);
                    break;
                }
                if (hit == SEARCH_HIT_BTN_REPL_ALL) {
                    search_replace_all(a);
                    break;
                }
                if (hit == SEARCH_HIT_INPUT || hit == SEARCH_HIT_REPLACE) {
                    /* Focus the field, then drop the caret where the click
                     * landed. We re-fetch geometry to know where the text
                     * begins (after the icon / "→" prefix). */
                    SDL_Rect input, replace;
                    search_geometry(a, NULL, &input, &replace,
                                    NULL, NULL, NULL, NULL);
                    a->search_focus_replace = (hit == SEARCH_HIT_REPLACE);
                    Font* f = a->font_body;
                    int sz_y = font_line_height(f);
                    int icon_sz = (input.h - 6);  /* matches render */
                    int text_x;
                    if (a->search_focus_replace) {
                        const char* lab = "→";
                        text_x = replace.x + 12
                               + font_measure(f, lab, strlen(lab)) + 8;
                    } else {
                        text_x = input.x + 6 + icon_sz + 6;
                    }
                    int x_rel = e->button.x - text_x;
                    const char* field = a->search_focus_replace
                                        ? a->search_replace : a->search_query;
                    size_t flen = a->search_focus_replace
                                  ? a->search_rlen : a->search_qlen;
                    size_t pos = text_byte_at_x(f, field, flen, x_rel);
                    if (a->search_focus_replace) a->search_rcursor = pos;
                    else                          a->search_qcursor = pos;
                    (void)sz_y;
                    break;
                }
                if (!search_bar_contains(a, e->button.x, e->button.y)) {
                    /* Clicked anywhere outside the bar — close so the
                     * editor / preview gets focus back. */
                    search_close(a);
                    /* Fall through so this click also acts on the editor
                     * (e.g. cursor placement). */
                }
            }
            if (a->vsearch_active && e->button.button == SDL_BUTTON_LEFT) {
                SDL_Rect vtrack, vthumb;
                if (vsearch_scrollbar_geom(a, &vtrack, &vthumb) &&
                    overlay_scrollbar_handle_click(a, e->button.x, e->button.y,
                        &vtrack, &vthumb, SB_VSEARCH,
                        &a->vsearch_scroll,
                        vsearch_row_h(a) * a->vsearch_count,
                        vsearch_row_h(a)))
                    break;
                int row = vsearch_hit_test(a, e->button.x, e->button.y);
                int box_w = VSEARCH_BOX_W;
                int box_x = (a->win_w - box_w) / 2;
                if (e->button.x < box_x ||
                    e->button.x >= box_x + box_w)
                {
                    vsearch_close(a);
                    break;
                }
                if (row >= 0 && a->vsearch_hits[row].line_no > 0) {
                    a->vsearch_selected = row;
                    vsearch_activate(a);
                }
                break;
            }
            /* Color picker overlay: click slot to select; click on a channel
             * cell to focus it. Click outside the box to close. */
            if (a->picker_active && e->button.button == SDL_BUTTON_LEFT) {
                SDL_Rect ptrack, pthumb;
                if (picker_scrollbar_geom(a, &ptrack, &pthumb) &&
                    overlay_scrollbar_handle_click(a, e->button.x, e->button.y,
                        &ptrack, &pthumb, SB_PICKER,
                        &a->picker_scroll,
                        picker_row_h(a) * COLOR_SLOT_COUNT,
                        picker_row_h(a)))
                    break;
                bool inside = false;
                int  ch     = -1;
                int  row    = picker_hit_test(a, e->button.x, e->button.y,
                                              &ch, &inside);
                if (!inside) { picker_close(a); break; }
                if (row >= 0) {
                    a->picker_selected = row;
                    if (ch >= 0) a->picker_channel = ch;
                }
                break;
            }
            /* Keybindings overlay: scrollbar grab takes priority, then row
             * click, then click-outside-to-close. */
            if (a->keybind_active && e->button.button == SDL_BUTTON_LEFT) {
                SDL_Rect ktrack, kthumb;
                if (keybind_scrollbar_geom(a, &ktrack, &kthumb) &&
                    overlay_scrollbar_handle_click(a,
                        e->button.x, e->button.y,
                        &ktrack, &kthumb, SB_KEYBIND,
                        &a->keybind_scroll, g_kbind_content_h,
                        kbind_row_h(a)))
                    break;
                int row = keybind_hit_test(a, e->button.x, e->button.y);
                if (row < 0) {
                    /* Outside the rows. If outside the box entirely, close. */
                    int box_w = KBIND_BOX_W;
                    int box_x = (a->win_w - box_w) / 2;
                    if (e->button.x < box_x ||
                        e->button.x >= box_x + box_w)
                    {
                        keybind_close(a);
                        settings_persist(a);
                    }
                    break;
                }
                a->keybind_selected = row;
                a->keybind_capturing = true;
                break;
            }
            /* Settings overlay: click chevrons to adjust, click row to select,
             * click outside to close (auto-save). */
            if (a->settings_active && e->button.button == SDL_BUTTON_LEFT) {
                bool inside = false;
                char part = 'B';
                int r = settings_hit_test(a, e->button.x, e->button.y,
                                          &part, &inside);
                if (!inside) { settings_close(a); break; }
                if (r >= 0) {
                    a->settings_selected = r;
                    if      (part == 'L') settings_adjust(a, (SettingsRow)r, -1);
                    else if (part == 'R') settings_adjust(a, (SettingsRow)r, +1);
                }
                break;
            }
            /* Help overlay: click outside closes it. */
            if (a->help_active && e->button.button == SDL_BUTTON_LEFT) {
                int box_w = 580;
                int box_x = (a->win_w - box_w) / 2;
                if (e->button.x < box_x ||
                    e->button.x >= box_x + box_w) {
                    help_close(a);
                    break;
                }
            }
            /* Right-click in the sidebar opens the sidebar context menu;
             * right-click in the editor (edit mode) opens the formatting
             * menu. Anywhere else closes any open menu. */
            if (e->button.button == SDL_BUTTON_RIGHT) {
                if (a->sidebar_open && e->button.x < a->sidebar_w) {
                    int idx = sidebar_item_at(a, e->button.x, e->button.y);
                    ctx_menu_open(a, e->button.x, e->button.y, idx);
                } else if (a->edit_mode &&
                           e->button.x >= doc_x_left(a) &&
                           e->button.x <  doc_x_right(a)) {
                    ctx_menu_open_editor(a, e->button.x, e->button.y);
                } else {
                    ctx_menu_close(a);
                }
                break;
            }
            /* Left-click on the open context menu chooses or dismisses.
             * Submenu rows are checked first so a click on a recent-vault
             * entry doesn't fall through to the parent dismiss path. */
            if (a->ctx_menu_active && e->button.button == SDL_BUTTON_LEFT) {
                if (a->ctx_submenu_active) {
                    int srow = submenu_row_at(a, e->button.x, e->button.y);
                    if (srow >= 0) { submenu_invoke_row(a, srow); break; }
                }
                int row = ctx_menu_row_at(a, e->button.x, e->button.y);
                if (row >= 0) {
                    ctx_menu_invoke_row(a, row);
                } else {
                    ctx_menu_close(a);
                }
                break;
            }
            if (e->button.button == SDL_BUTTON_LEFT) {
                /* Sidebar resize handle: pin starts a drag, swallows click. */
                if (a->sidebar_open &&
                    abs(e->button.x - a->sidebar_w) <= 3) {
                    a->resizing_sidebar = true;
                    break;
                }
                /* Outline panel resize handle (left edge of panel). */
                if (a->outline_pinned &&
                    abs(e->button.x - (a->win_w - a->outline_panel_w)) <= 3) {
                    a->resizing_outline = true;
                    break;
                }
                /* Document scrollbar: grab thumb to drag, click track to jump,
                 * click ▲/▼ to step by one line. */
                {
                    SDL_Rect track, thumb;
                    if (doc_scrollbar_geom(a, &track, &thumb) &&
                        overlay_scrollbar_handle_click(a,
                            e->button.x, e->button.y,
                            &track, &thumb, SB_DOC,
                            &a->scroll_y, a->doc_height_px,
                            line_step(a, a->font_body)))
                    {
                        clamp_scroll(a);
                        break;
                    }
                }
                int idx = sidebar_item_at(a, e->button.x, e->button.y);
                if (idx >= 0 && idx < (int)a->vault.count) {
                    VaultItem* it = &a->vault.items[idx];
                    if (it->is_dir) {
                        it->collapsed = !it->collapsed;
                    } else {
                        /* Arm DnD: defer load_note to mouse-up so we can
                         * tell click vs drag. */
                        a->dnd_armed       = true;
                        a->dnd_source_idx  = idx;
                        a->dnd_press_x     = e->button.x;
                        a->dnd_press_y     = e->button.y;
                        a->dnd_drop_target = -1;
                    }
                } else if (a->edit_mode &&
                           e->button.x >= doc_x_left(a)) {
                    /* Click in the editor closes any active wiki-complete. */
                    if (a->wc_active) wc_close(a);
                    size_t pos = edit_position_at(a, e->button.x, e->button.y);
                    bool ctrlmod = (SDL_GetModState() & KMOD_CTRL) != 0;
                    /* Ctrl+click on a wiki-link follows it; cursor doesn't
                     * move and the click doesn't start a drag-select. */
                    if (ctrlmod) {
                        char name[256];
                        if (edit_wiki_link_at(&a->buf, pos,
                                              name, sizeof name) > 0 &&
                            follow_wiki_target(a, name))
                            break;
                    }
                    bool select = (SDL_GetModState() & KMOD_SHIFT) != 0;
                    /* Multi-click: 2 = word, 3 = whole line. */
                    if (e->button.clicks == 3) {
                        size_t ls = pos, le = pos;
                        while (ls > 0 && a->buf.data[ls-1] != '\n') ls--;
                        while (le < a->buf.len && a->buf.data[le] != '\n') le++;
                        if (le < a->buf.len) le++;     /* include the \n */
                        a->buf.sel_anchor = (long)ls;
                        a->buf.cursor     = le;
                        a->mouse_selecting = false;
                    } else if (e->button.clicks == 2) {
                        size_t lo = pos, hi = pos;
                        while (lo > 0 && !isspace((unsigned char)a->buf.data[lo-1]))
                            lo--;
                        while (hi < a->buf.len &&
                               !isspace((unsigned char)a->buf.data[hi])) hi++;
                        a->buf.sel_anchor = (long)lo;
                        a->buf.cursor     = hi;
                        a->mouse_selecting = false;
                    } else {
                        buffer_set_cursor(&a->buf, pos, select);
                        /* Set anchor so subsequent drag-motion extends from
                         * here. (Shift+click already kept the prior anchor.) */
                        if (!select) a->buf.sel_anchor = (long)pos;
                        a->mouse_selecting = true;
                    }
                    bump_blink(a);
                } else if (!a->edit_mode &&
                           e->button.x >= doc_x_left(a)) {
                    /* Frontmatter chip click → vault-search for that tag.
                     * Hit-tested first because chips sit above the doc text. */
                    int chip_clicked = 0;
                    for (int ci = 0; ci < a->fm_chip_count; ++ci) {
                        struct FmChip* h = &a->fm_chip_hits[ci];
                        if (e->button.x >= h->rect.x &&
                            e->button.x <  h->rect.x + h->rect.w &&
                            e->button.y >= h->rect.y &&
                            e->button.y <  h->rect.y + h->rect.h)
                        {
                            snprintf(a->vsearch_query,
                                     sizeof a->vsearch_query, "#%s", h->tag);
                            a->vsearch_qlen  = strlen(a->vsearch_query);
                            a->vsearch_regex = false;
                            vsearch_open(a);
                            vsearch_rebuild(a);
                            chip_clicked = 1;
                            break;
                        }
                    }
                    if (chip_clicked) break;
                    /* Try wiki-link / task-list click navigation first; if
                     * no hit, fall through to start a preview drag-select.
                     * Ctrl+click on a link byte resolves through the inline
                     * `[text](url)` table and opens externally (after a
                     * confirm prompt) instead of treating it as a wiki. */
                    bool ctrl_held = (SDL_GetModState() & KMOD_CTRL) != 0;
                    int handled = 0;
                    for (size_t hi = 0; hi < a->hit_count; ++hi) {
                        struct ClickHit* h = &a->hits[hi];
                        if (e->button.x < h->rect.x ||
                            e->button.x >= h->rect.x + h->rect.w ||
                            e->button.y < h->rect.y ||
                            e->button.y >= h->rect.y + h->rect.h) continue;
                        if (h->kind == HIT_TASK) {
                            /* Toggle the byte: ' ' ↔ 'x' (preserve 'X'). */
                            size_t off = h->byte_start;
                            if (off < a->buf.len) {
                                char* p = &a->buf.data[off];
                                if (*p == ' ')      *p = 'x';
                                else if (*p == 'x') *p = ' ';
                                else if (*p == 'X') *p = ' ';
                                a->buf.dirty = true;
                                buffer_undo_break(&a->buf);
                                md_doc_free(&a->doc);
                                md_doc_parse(a->buf.data, a->buf.len, &a->doc);
                                update_window_title(a);
                            }
                            handled = 1;
                            goto out_of_hits;
                        }
                        if (ctrl_held) {
                            for (size_t li = 0; li < a->doc.link_count; ++li) {
                                MdLink* lk = &a->doc.links[li];
                                if (h->byte_start < lk->start ||
                                    h->byte_start >= lk->end) continue;
                                if (!is_external_url(lk->href)) break;
                                char msg[300];
                                snprintf(msg, sizeof msg,
                                    "Open in default browser?\n%.220s",
                                    lk->href);
                                if (confirm_action(a, "Open link",
                                                   msg, "Open", "Cancel"))
                                {
                                    open_external_url(lk->href);
                                }
                                handled = 1;
                                goto out_of_hits;
                            }
                        }
                        for (size_t wi = 0; wi < a->doc.wiki_count; ++wi) {
                            MdWiki* wk = &a->doc.wikis[wi];
                            if (h->byte_start < wk->start ||
                                h->byte_start >= wk->end) continue;
                            char target[256];
                            size_t n = wk->name_len < sizeof target - 1
                                        ? wk->name_len : sizeof target - 1;
                            memcpy(target, a->doc.data + wk->name_start, n);
                            target[n] = 0;
                            /* Find a vault item whose basename (sans .md)
                             * matches the wiki target, case-insensitively. */
                            for (size_t v = 0; v < a->vault.count; ++v) {
                                if (a->vault.items[v].is_dir) continue;
                                char base[256];
                                snprintf(base, sizeof base, "%s",
                                         a->vault.items[v].name);
                                size_t bl = strlen(base);
                                if (bl > 3 && strieq(base + bl - 3, ".md"))
                                    base[bl - 3] = 0;
                                if (strieq(base, target)) {
                                    if (!confirm_discard(a)) goto wiki_done;
                                    load_note(a, a->vault.items[v].path);
                                    goto wiki_done;
                                }
                            }
                            fprintf(stderr,
                                "wiki: no vault item matches [[%s]]\n", target);
                            wiki_done:
                            handled = 1;
                            goto out_of_hits;
                        }
                    }
                    out_of_hits:
                    if (!handled) {
                        /* Start a preview-mode drag-select. */
                        size_t pos = preview_position_at(a, e->button.x,
                                                        e->button.y);
                        a->preview_sel_start = (long)pos;
                        a->preview_sel_end   = pos;
                        a->preview_selecting = true;
                    }
                }
            }
            break;

        case SDL_MOUSEWHEEL: {
            int mx, my;
            SDL_GetMouseState(&mx, &my);
            /* Settings: wheel adjusts the selected row (or row under cursor). */
            if (a->settings_active) {
                int r = settings_hit_test(a, mx, my, NULL, NULL);
                if (r < 0) r = a->settings_selected;
                if (r >= 0 && r < SET_COUNT) {
                    a->settings_selected = r;
                    settings_adjust(a, (SettingsRow)r,
                                    e->wheel.y > 0 ? +1 : -1);
                }
                break;
            }
            /* Plugins overlay: wheel scrolls the body. */
            if (a->plugins_active) {
                a->plugins_scroll -= e->wheel.y * 40;
                if (a->plugins_scroll < 0) a->plugins_scroll = 0;
                break;
            }
            /* Command palette: wheel scrolls the row band. */
            if (a->cmdp_active) {
                a->cmdp_scroll -= e->wheel.y * cmdp_row_h(a) * 2;
                cmdp_clamp_scroll(a);
                break;
            }
            /* Help: wheel scrolls the list. */
            if (a->help_active) {
                a->help_scroll -= e->wheel.y * 40;
                if (a->help_scroll < 0) a->help_scroll = 0;
                break;
            }
            /* Keybindings: wheel scrolls the list. Clamp to content height
             * so the user can't scroll into infinity past the rows. */
            if (a->keybind_active) {
                a->keybind_scroll -= e->wheel.y * 40;
                if (a->keybind_scroll < 0) a->keybind_scroll = 0;
                SDL_Rect ktrack, kthumb;
                if (keybind_scrollbar_geom(a, &ktrack, &kthumb)) {
                    int max_sc = g_kbind_content_h - ktrack.h;
                    if (max_sc < 0) max_sc = 0;
                    if (a->keybind_scroll > max_sc) a->keybind_scroll = max_sc;
                } else {
                    a->keybind_scroll = 0;     /* nothing to scroll */
                }
                break;
            }
            /* Vault search: wheel scrolls the result list. */
            if (a->vsearch_active) {
                a->vsearch_scroll -= e->wheel.y * 40;
                if (a->vsearch_scroll < 0) a->vsearch_scroll = 0;
                break;
            }
            /* Outline: wheel scrolls. Active overlay AND pinned panel both
             * use a->outline_scroll, so a single branch works for both. */
            if (a->outline_active) {
                a->outline_scroll -= e->wheel.y * 40;
                if (a->outline_scroll < 0) a->outline_scroll = 0;
                break;
            }
            if (a->outline_pinned && mx >= outline_panel_x(a)) {
                a->outline_scroll -= e->wheel.y * 40;
                if (a->outline_scroll < 0) a->outline_scroll = 0;
                break;
            }
            /* Backlinks / tags / template panels: wheel scrolls. */
            if (a->backlinks_active) {
                a->backlinks_scroll -= e->wheel.y * 40;
                if (a->backlinks_scroll < 0) a->backlinks_scroll = 0;
                break;
            }
            if (a->tags_active) {
                a->tags_scroll -= e->wheel.y * 40;
                if (a->tags_scroll < 0) a->tags_scroll = 0;
                break;
            }
            if (a->tpl_active) {
                a->tpl_scroll -= e->wheel.y * 40;
                if (a->tpl_scroll < 0) a->tpl_scroll = 0;
                break;
            }
            /* Color picker: wheel adjusts the focused channel by ±5 (or ±1
             * with Shift held), and snaps focus to the row under the
             * cursor first so the user can scroll on a different row
             * without first clicking it. */
            if (a->picker_active) {
                int r = picker_hit_test(a, mx, my, NULL, NULL);
                if (r >= 0) a->picker_selected = r;
                int delta = (SDL_GetModState() & KMOD_SHIFT) ? 1 : 5;
                picker_adjust(a, e->wheel.y > 0 ? +delta : -delta);
                break;
            }
            if (a->sidebar_open && mx < a->sidebar_w) {
                a->sidebar_scroll_y -=
                    e->wheel.y * sidebar_item_height(a) * 2;
                if (a->sidebar_scroll_y < 0) a->sidebar_scroll_y = 0;
                int m = sidebar_max_scroll(a);
                if (a->sidebar_scroll_y > m) a->sidebar_scroll_y = m;
                break;
            }
            a->scroll_y -= e->wheel.y * line_px * 3;
            clamp_scroll(a);
            break;
        }

        case SDL_TEXTINPUT:
            if (a->vsearch_active) {
                size_t in = strlen(e->text.text);
                if (a->vsearch_qlen + in < sizeof(a->vsearch_query) - 1) {
                    memcpy(a->vsearch_query + a->vsearch_qlen,
                           e->text.text, in);
                    a->vsearch_qlen += in;
                    a->vsearch_query[a->vsearch_qlen] = 0;
                    vsearch_rebuild(a);
                }
                break;
            }
            if (a->switcher_active) {
                size_t in = strlen(e->text.text);
                if (a->switcher_qlen + in < sizeof(a->switcher_query) - 1) {
                    memcpy(a->switcher_query + a->switcher_qlen,
                           e->text.text, in);
                    a->switcher_qlen += in;
                    a->switcher_query[a->switcher_qlen] = 0;
                    switcher_rebuild(a);
                }
                break;
            }
            if (a->cmdp_active) {
                size_t in = strlen(e->text.text);
                if (a->cmdp_qlen + in < sizeof(a->cmdp_query) - 1) {
                    memcpy(a->cmdp_query + a->cmdp_qlen,
                           e->text.text, in);
                    a->cmdp_qlen += in;
                    a->cmdp_query[a->cmdp_qlen] = 0;
                    cmdp_rebuild(a);
                }
                break;
            }
            if (a->search_mode != 0) {
                char*   field = a->search_focus_replace
                                ? a->search_replace : a->search_query;
                size_t* flen  = a->search_focus_replace
                                ? &a->search_rlen : &a->search_qlen;
                size_t* fcur  = a->search_focus_replace
                                ? &a->search_rcursor : &a->search_qcursor;
                size_t  in_n  = strlen(e->text.text);
                if (*flen + in_n < 255) {
                    /* Insert at caret instead of appending so the user can
                     * edit the middle of the string. */
                    if (*fcur > *flen) *fcur = *flen;
                    memmove(field + *fcur + in_n, field + *fcur,
                            *flen - *fcur + 1);
                    memcpy(field + *fcur, e->text.text, in_n);
                    *flen += in_n;
                    *fcur += in_n;
                }
                if (!a->search_focus_replace) search_rebuild(a);
                break;
            }
            if (a->edit_mode) {
                const char* t  = e->text.text;
                size_t      tn = strlen(t);
                bool handled = false;
                /* Auto-pair / skip-over only fires for single ASCII chars.
                 * Multi-byte input (IME, paste) goes through the normal path. */
                if (tn == 1 && !buffer_has_selection(&a->buf)) {
                    char  c    = t[0];
                    char  next = (a->buf.cursor < a->buf.len)
                                 ? a->buf.data[a->buf.cursor] : 0;
                    char  closer = pair_closer_for(c);
                    /* 1) Skip-over: typing `)` / `]` / `}` / `"` / `'` / `` ` ``
                     *    when that exact char is already to the right of the
                     *    cursor — common case after auto-pair: type `(`, type
                     *    body, type `)` to confirm. */
                    if (is_pair_closer_char(c) && next == c) {
                        a->buf.cursor++;
                        a->buf.sel_anchor = -1;
                        handled = true;
                    }
                    /* 2) Auto-pair: typing an opener inserts the closer and
                     *    leaves the cursor between them. Skipped if the next
                     *    char is alphanumeric (avoid breaking `foo(` → `foo()`
                     *    when the user actually wants to call as `foo(arg`).
                     *    Symmetric pairs (`"`, `'`, `` ` ``) also skip if the
                     *    previous char is a word char (closing a quote
                     *    directly after a word, like `it's`). */
                    else if (closer && !is_word_char((unsigned char)next)) {
                        bool symmetric = (c == closer);
                        char prev = (a->buf.cursor > 0)
                                    ? a->buf.data[a->buf.cursor - 1] : 0;
                        if (symmetric && is_word_char((unsigned char)prev)) {
                            /* Likely closing/owning a word, e.g. `don't` —
                             * fall through to plain insert. */
                        } else {
                            char pair[2] = { c, closer };
                            buffer_insert(&a->buf, pair, 2);
                            buffer_move_left(&a->buf, false);
                            handled = true;
                        }
                    }
                }
                if (!handled) buffer_insert(&a->buf, t, tn);
                update_window_title(a);
                ensure_cursor_visible(a);
                bump_blink(a);
                a->notification_until = 0;     /* dismiss on type */

                /* Wiki-complete activation: did the user just type the second
                 * `[` of `[[`? After auto-pair, buf has `...[[]]` with the
                 * cursor between the inner pair. */
                if (!a->wc_active && tn == 1 && t[0] == '[' &&
                    a->buf.cursor >= 2 && a->buf.cursor + 2 <= a->buf.len &&
                    a->buf.data[a->buf.cursor - 2] == '[' &&
                    a->buf.data[a->buf.cursor - 1] == '[' &&
                    a->buf.data[a->buf.cursor    ] == ']' &&
                    a->buf.data[a->buf.cursor + 1] == ']')
                {
                    int cx, cy;
                    edit_cursor_screen_pos(a, &cx, &cy);
                    wc_open_at_cursor(a, cx, cy);
                }
                else if (a->wc_active) {
                    /* Cursor moved past the auto-paired `]]` boundary, or the
                     * user typed something that broke the link — e.g. a `]`,
                     * a newline, or moved before the anchor. Otherwise just
                     * refresh the filter from buf[anchor..cursor]. */
                    if (a->buf.cursor < a->wc_anchor) wc_close(a);
                    else if (tn == 1 && (t[0] == ']' || t[0] == '\n'))
                        wc_close(a);
                    else wc_rebuild(a);
                }
            }
            break;

        case SDL_KEYDOWN: {
            SDL_Keycode k = e->key.keysym.sym;
            int mod  = e->key.keysym.mod;
            bool ctrl = (mod & KMOD_CTRL)  != 0;
            bool sel  = (mod & KMOD_SHIFT) != 0;

            /* Backlinks panel swallows nav keys while open. */
            if (a->backlinks_active) {
                if (k == SDLK_ESCAPE) { backlinks_close(a); break; }
                if (a->backlinks_count == 0) break;
                if (k == SDLK_DOWN) {
                    a->backlinks_selected =
                        (a->backlinks_selected + 1) % a->backlinks_count;
                    break;
                }
                if (k == SDLK_UP) {
                    a->backlinks_selected =
                        (a->backlinks_selected - 1 + a->backlinks_count) %
                        a->backlinks_count;
                    break;
                }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    backlinks_activate(a);
                    break;
                }
                break;
            }
            /* Tag panel swallows nav keys while open. */
            if (a->tags_active) {
                if (k == SDLK_ESCAPE) { tags_close(a); break; }
                if (a->tags_count == 0) break;
                if (k == SDLK_DOWN) {
                    a->tags_selected =
                        (a->tags_selected + 1) % a->tags_count;
                    break;
                }
                if (k == SDLK_UP) {
                    a->tags_selected =
                        (a->tags_selected - 1 + a->tags_count) % a->tags_count;
                    break;
                }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    tags_activate(a);
                    break;
                }
                break;
            }
            /* Template picker swallows nav keys while open. */
            if (a->tpl_active) {
                if (k == SDLK_ESCAPE) { tpl_close(a); break; }
                if (a->tpl_count == 0) break;
                if (k == SDLK_DOWN) {
                    a->tpl_selected =
                        (a->tpl_selected + 1) % a->tpl_count;
                    break;
                }
                if (k == SDLK_UP) {
                    a->tpl_selected =
                        (a->tpl_selected - 1 + a->tpl_count) % a->tpl_count;
                    break;
                }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    tpl_activate(a);
                    break;
                }
                break;
            }

            /* Outline overlay swallows nav keys while open. */
            if (a->outline_active) {
                if (k == SDLK_ESCAPE) { outline_close(a); break; }
                if (a->outline_count == 0) break;
                if (k == SDLK_DOWN) {
                    a->outline_selected =
                        (a->outline_selected + 1) % a->outline_count;
                    break;
                }
                if (k == SDLK_UP) {
                    a->outline_selected =
                        (a->outline_selected - 1 + a->outline_count) %
                        a->outline_count;
                    break;
                }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    outline_activate(a);
                    break;
                }
                if (k == SDLK_HOME) {
                    a->outline_selected = 0;
                    break;
                }
                if (k == SDLK_END) {
                    a->outline_selected = a->outline_count - 1;
                    break;
                }
                if (k == SDLK_PAGEDOWN) {
                    a->outline_scroll += 5 * outline_row_h(a);
                    break;
                }
                if (k == SDLK_PAGEUP) {
                    a->outline_scroll -= 5 * outline_row_h(a);
                    if (a->outline_scroll < 0) a->outline_scroll = 0;
                    break;
                }
                break;     /* swallow everything else */
            }

            /* Vault search overlay swallows keys while open. */
            if (a->vsearch_active) {
                if (k == SDLK_ESCAPE) { vsearch_close(a); break; }
                bool altmod = (mod & KMOD_ALT) != 0;
                if (altmod && k == SDLK_r) {
                    a->vsearch_regex = !a->vsearch_regex;
                    vsearch_rebuild(a);
                    break;
                }
                if (altmod && k == SDLK_i) {
                    a->vsearch_ci = !a->vsearch_ci;
                    vsearch_rebuild(a);
                    break;
                }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    vsearch_activate(a);
                    break;
                }
                if (k == SDLK_DOWN) { vsearch_move(a, +1); break; }
                if (k == SDLK_UP)   { vsearch_move(a, -1); break; }
                if (k == SDLK_BACKSPACE) {
                    if (a->vsearch_qlen > 0) {
                        do { a->vsearch_qlen--; }
                        while (a->vsearch_qlen > 0 &&
                               ((unsigned char)a->vsearch_query[a->vsearch_qlen]
                                & 0xC0) == 0x80);
                        a->vsearch_query[a->vsearch_qlen] = 0;
                        vsearch_rebuild(a);
                    }
                    break;
                }
                if (k == SDLK_PAGEDOWN) {
                    a->vsearch_scroll += 5 * vsearch_row_h(a);
                    break;
                }
                if (k == SDLK_PAGEUP) {
                    a->vsearch_scroll -= 5 * vsearch_row_h(a);
                    if (a->vsearch_scroll < 0) a->vsearch_scroll = 0;
                    break;
                }
                break;     /* swallow everything else */
            }

            /* Switcher swallows keys while open. */
            if (a->switcher_active) {
                if (k == SDLK_ESCAPE) { switcher_close(a); break; }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    switcher_select(a); break;
                }
                if (k == SDLK_UP) {
                    if (a->switcher_selected > 0) a->switcher_selected--;
                    break;
                }
                if (k == SDLK_DOWN) {
                    if (a->switcher_selected + 1 < a->switcher_count)
                        a->switcher_selected++;
                    break;
                }
                if (k == SDLK_BACKSPACE) {
                    if (a->switcher_qlen > 0) {
                        do { a->switcher_qlen--; }
                        while (a->switcher_qlen > 0 &&
                               ((unsigned char)a->switcher_query[a->switcher_qlen]
                                & 0xC0) == 0x80);
                        a->switcher_query[a->switcher_qlen] = 0;
                        switcher_rebuild(a);
                    }
                    break;
                }
                break;     /* swallow everything else */
            }

            /* Plugins overlay swallows keys while open. */
            if (a->plugins_active) {
                if (k == SDLK_ESCAPE) { plugins_close(a); break; }
                if (k == SDLK_F5)     { plugins_action_reload(a); break; }
                int rh = plugins_row_h(a);
                if (k == SDLK_UP) {
                    a->plugins_scroll -= rh * 2;
                    if (a->plugins_scroll < 0) a->plugins_scroll = 0;
                    break;
                }
                if (k == SDLK_DOWN) {
                    a->plugins_scroll += rh * 2;
                    break;
                }
                if (k == SDLK_PAGEUP) {
                    a->plugins_scroll -= rh * 10;
                    if (a->plugins_scroll < 0) a->plugins_scroll = 0;
                    break;
                }
                if (k == SDLK_PAGEDOWN) {
                    a->plugins_scroll += rh * 10;
                    break;
                }
                break;
            }

            /* Command palette swallows keys while open. */
            if (a->cmdp_active) {
                if (k == SDLK_ESCAPE) { cmdp_close(a); break; }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    cmdp_invoke(a); break;
                }
                if (k == SDLK_UP) {
                    if (a->cmdp_selected > 0) a->cmdp_selected--;
                    cmdp_ensure_selected_visible(a);
                    break;
                }
                if (k == SDLK_DOWN) {
                    if (a->cmdp_selected + 1 < a->cmdp_count)
                        a->cmdp_selected++;
                    cmdp_ensure_selected_visible(a);
                    break;
                }
                if (k == SDLK_PAGEUP) {
                    a->cmdp_selected -= 10;
                    if (a->cmdp_selected < 0) a->cmdp_selected = 0;
                    cmdp_ensure_selected_visible(a);
                    break;
                }
                if (k == SDLK_PAGEDOWN) {
                    a->cmdp_selected += 10;
                    if (a->cmdp_selected >= a->cmdp_count)
                        a->cmdp_selected = a->cmdp_count - 1;
                    cmdp_ensure_selected_visible(a);
                    break;
                }
                if (k == SDLK_HOME) {
                    a->cmdp_selected = a->cmdp_count > 0 ? 0 : -1;
                    a->cmdp_scroll = 0;
                    break;
                }
                if (k == SDLK_END) {
                    a->cmdp_selected = a->cmdp_count - 1;
                    cmdp_ensure_selected_visible(a);
                    break;
                }
                if (k == SDLK_BACKSPACE) {
                    if (a->cmdp_qlen > 0) {
                        do { a->cmdp_qlen--; }
                        while (a->cmdp_qlen > 0 &&
                               ((unsigned char)a->cmdp_query[a->cmdp_qlen]
                                & 0xC0) == 0x80);
                        a->cmdp_query[a->cmdp_qlen] = 0;
                        cmdp_rebuild(a);
                    }
                    break;
                }
                break;
            }

            /* Wiki-complete swallows navigation keys while open. */
            if (a->wc_active) {
                if (k == SDLK_ESCAPE)   { wc_close(a); break; }
                if (k == SDLK_TAB ||
                    k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    wc_select(a);
                    update_window_title(a);
                    ensure_cursor_visible(a);
                    bump_blink(a);
                    break;
                }
                if (k == SDLK_UP) {
                    if (a->wc_selected > 0) a->wc_selected--;
                    break;
                }
                if (k == SDLK_DOWN) {
                    if (a->wc_selected + 1 < a->wc_count) a->wc_selected++;
                    break;
                }
                if (k == SDLK_BACKSPACE) {
                    /* If the cursor is right at the anchor, backspacing would
                     * eat the second `[` — close the popover and let the
                     * normal backspace path run, which will also delete the
                     * auto-paired `]` (well, only the `[`; we leave that as
                     * existing buffer behavior). */
                    if (a->buf.cursor <= a->wc_anchor) {
                        wc_close(a);
                        /* fall through to normal handling */
                    } else {
                        buffer_delete_back(&a->buf);
                        wc_rebuild(a);
                        update_window_title(a);
                        ensure_cursor_visible(a);
                        bump_blink(a);
                        break;
                    }
                }
                /* Arrow keys other than Up/Down (Left/Right) and any other
                 * navigation in edit mode → close popover, let key fall
                 * through. */
                if (k == SDLK_LEFT || k == SDLK_RIGHT ||
                    k == SDLK_HOME || k == SDLK_END   ||
                    k == SDLK_PAGEUP || k == SDLK_PAGEDOWN)
                {
                    wc_close(a);
                    /* fall through */
                }
            }

            /* Help overlay swallows nav keys while open. */
            if (a->help_active) {
                if (k == SDLK_ESCAPE)   { help_close(a); break; }
                if (k == SDLK_DOWN || k == SDLK_PAGEDOWN) {
                    a->help_scroll += 40; break;
                }
                if (k == SDLK_UP || k == SDLK_PAGEUP) {
                    a->help_scroll -= 40;
                    if (a->help_scroll < 0) a->help_scroll = 0;
                    break;
                }
                break;     /* swallow everything else */
            }

            /* Color picker overlay swallows nav keys while open. */
            if (a->picker_active) {
                if (k == SDLK_ESCAPE) { picker_close(a); break; }
                if (k == SDLK_DOWN) {
                    a->picker_selected =
                        (a->picker_selected + 1) % COLOR_SLOT_COUNT;
                    break;
                }
                if (k == SDLK_UP) {
                    a->picker_selected =
                        (a->picker_selected - 1 + COLOR_SLOT_COUNT) % COLOR_SLOT_COUNT;
                    break;
                }
                if (k == SDLK_TAB) {
                    int dir = (mod & KMOD_SHIFT) ? -1 : 1;
                    a->picker_channel = (a->picker_channel + dir + 4) % 4;
                    break;
                }
                if (k == SDLK_LEFT) {
                    a->picker_channel = (a->picker_channel + 3) % 4;
                    break;
                }
                if (k == SDLK_RIGHT) {
                    a->picker_channel = (a->picker_channel + 1) % 4;
                    break;
                }
                /* Adjust: +/- adjust by 5; Shift+ for ±1; PageUp/Down also ±25. */
                if (k == SDLK_PLUS  || k == SDLK_EQUALS ||
                    k == SDLK_KP_PLUS) {
                    picker_adjust(a, (mod & KMOD_SHIFT) ? 1 : 5);
                    break;
                }
                if (k == SDLK_MINUS || k == SDLK_KP_MINUS) {
                    picker_adjust(a, (mod & KMOD_SHIFT) ? -1 : -5);
                    break;
                }
                if (k == SDLK_PAGEUP)   { picker_adjust(a,  25); break; }
                if (k == SDLK_PAGEDOWN) { picker_adjust(a, -25); break; }
                if (k == SDLK_HOME)     { picker_adjust(a, -255); break; }
                if (k == SDLK_END)      { picker_adjust(a,  255); break; }
                break;     /* swallow everything else */
            }

            /* Keybindings overlay swallows everything while open. */
            if (a->keybind_active) {
                if (a->keybind_capturing) {
                    /* Esc cancels the capture (keeps overlay open). */
                    if (k == SDLK_ESCAPE) {
                        a->keybind_capturing = false;
                        break;
                    }
                    /* Ignore standalone modifier keys; wait for a "real"
                     * key. Otherwise capture and bind. */
                    if (k == SDLK_LCTRL || k == SDLK_RCTRL ||
                        k == SDLK_LSHIFT || k == SDLK_RSHIFT ||
                        k == SDLK_LALT || k == SDLK_RALT ||
                        k == SDLK_LGUI || k == SDLK_RGUI) break;
                    char ks[64];
                    build_keystr(k, mod, ks, sizeof ks);
                    if (a->keybind_selected >= 0 &&
                        a->keybind_selected < ACTIONS_count())
                    {
                        const char* act = ACTIONS_name(a->keybind_selected);
                        user_kbind_set(act, ks);
                        char msg[200];
                        snprintf(msg, sizeof msg,
                            "%s bound to %s", act, ks);
                        app_notify(a, msg);
                    }
                    a->keybind_capturing = false;
                    break;
                }
                if (k == SDLK_ESCAPE) {
                    keybind_close(a);
                    /* Persist on close so user changes survive a restart. */
                    settings_persist(a);
                    break;
                }
                int n = ACTIONS_count();
                if (n == 0) break;
                if (k == SDLK_DOWN) {
                    a->keybind_selected = (a->keybind_selected + 1) % n;
                    keybind_ensure_selected_visible(a);
                    break;
                }
                if (k == SDLK_UP) {
                    a->keybind_selected = (a->keybind_selected - 1 + n) % n;
                    keybind_ensure_selected_visible(a);
                    break;
                }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    a->keybind_capturing = true;
                    break;
                }
                if (k == SDLK_DELETE || k == SDLK_BACKSPACE) {
                    if (a->keybind_selected >= 0 && a->keybind_selected < n) {
                        const char* act = ACTIONS_name(a->keybind_selected);
                        user_kbind_set(act, "");
                        app_notify(a, "binding cleared (default restored)");
                    }
                    break;
                }
                break;     /* swallow everything else */
            }

            /* Settings overlay swallows nav keys while open. */
            if (a->settings_active) {
                if (k == SDLK_ESCAPE) { settings_close(a); break; }
                if (k == SDLK_DOWN) {
                    a->settings_selected = (a->settings_selected + 1) % SET_COUNT;
                    break;
                }
                if (k == SDLK_UP) {
                    a->settings_selected =
                        (a->settings_selected - 1 + SET_COUNT) % SET_COUNT;
                    break;
                }
                if (k == SDLK_LEFT) {
                    settings_adjust(a,
                        (SettingsRow)a->settings_selected, -1);
                    break;
                }
                if (k == SDLK_RIGHT) {
                    settings_adjust(a,
                        (SettingsRow)a->settings_selected, +1);
                    break;
                }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    /* Enter cycles forward (handy for cycling fonts). */
                    settings_adjust(a,
                        (SettingsRow)a->settings_selected, +1);
                    break;
                }
                break;     /* swallow everything else */
            }

            /* Context menu swallows nav keys while open. */
            if (a->ctx_menu_active) {
                int n = ctx_visible_count(a);
                if (k == SDLK_DOWN) {
                    if (n > 0) a->ctx_menu_hover = (a->ctx_menu_hover + 1) % n;
                    break;
                }
                if (k == SDLK_UP) {
                    if (n > 0) a->ctx_menu_hover =
                        (a->ctx_menu_hover - 1 + n) % n;
                    break;
                }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    if (a->ctx_menu_hover >= 0 && a->ctx_menu_hover < n)
                        ctx_menu_invoke_row(a, a->ctx_menu_hover);
                    else
                        ctx_menu_close(a);
                    break;
                }
            }

            /* Esc with active search → close search. Otherwise mode-dep. */
            if (k == SDLK_ESCAPE) {
                if (a->ctx_menu_active) { ctx_menu_close(a); break; }
                if (a->search_mode != 0) { search_close(a); break; }
                if (a->edit_mode) enter_preview_mode(a);
                else if (confirm_discard(a)) a->running = false;
                break;
            }

            /* Search overlay swallows most keys while open. */
            if (a->search_mode != 0) {
                bool altmod = (mod & KMOD_ALT) != 0;
                if (altmod && k == SDLK_i) {
                    a->search_case_insensitive = !a->search_case_insensitive;
                    search_rebuild(a);
                    break;
                }
                if (altmod && k == SDLK_w) {
                    a->search_whole_word = !a->search_whole_word;
                    search_rebuild(a);
                    break;
                }
                if (altmod && k == SDLK_r) {
                    a->search_regex = !a->search_regex;
                    search_rebuild(a);
                    break;
                }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    if (ctrl) {
                        if (sel) search_replace_all(a);
                        else     search_replace_one(a);
                    } else if (sel) search_prev(a);
                    else            search_next(a);
                } else if (k == SDLK_TAB && a->search_mode == 2) {
                    a->search_focus_replace = !a->search_focus_replace;
                } else {
                    /* Caret-aware editing of the focused field. Same byte
                     * arithmetic as buffer_*: walk back over UTF-8
                     * continuation bytes so we don't split a codepoint. */
                    char*   field = a->search_focus_replace
                                    ? a->search_replace : a->search_query;
                    size_t* flen  = a->search_focus_replace
                                    ? &a->search_rlen : &a->search_qlen;
                    size_t* fcur  = a->search_focus_replace
                                    ? &a->search_rcursor : &a->search_qcursor;
                    if (*fcur > *flen) *fcur = *flen;
                    if (k == SDLK_BACKSPACE) {
                        if (*fcur > 0) {
                            size_t step = 1;
                            while (step < *fcur &&
                                ((unsigned char)field[*fcur - step] & 0xC0) == 0x80)
                                step++;
                            memmove(field + *fcur - step,
                                    field + *fcur,
                                    *flen - *fcur + 1);
                            *flen -= step;
                            *fcur -= step;
                            if (!a->search_focus_replace) search_rebuild(a);
                        }
                    } else if (k == SDLK_DELETE) {
                        if (*fcur < *flen) {
                            size_t step = 1;
                            while (*fcur + step < *flen &&
                                ((unsigned char)field[*fcur + step] & 0xC0) == 0x80)
                                step++;
                            memmove(field + *fcur,
                                    field + *fcur + step,
                                    *flen - *fcur - step + 1);
                            *flen -= step;
                            if (!a->search_focus_replace) search_rebuild(a);
                        }
                    } else if (k == SDLK_LEFT) {
                        if (*fcur > 0) {
                            size_t step = 1;
                            while (step < *fcur &&
                                ((unsigned char)field[*fcur - step] & 0xC0) == 0x80)
                                step++;
                            *fcur -= step;
                        }
                    } else if (k == SDLK_RIGHT) {
                        if (*fcur < *flen) {
                            size_t step = 1;
                            while (*fcur + step < *flen &&
                                ((unsigned char)field[*fcur + step] & 0xC0) == 0x80)
                                step++;
                            *fcur += step;
                        }
                    } else if (k == SDLK_HOME) {
                        *fcur = 0;
                    } else if (k == SDLK_END) {
                        *fcur = *flen;
                    }
                }
                break;
            }

            /* Action lookup: user-set bindings (settings UI) override the
             * Lua keybindings table, which itself overrides DEFAULT_KEYS. */
            char keystr[64];
            build_keystr(k, mod, keystr, sizeof keystr);
            const char* aname = user_kbind_for(keystr);
            if (!aname) aname =
                lua_host_cfg_table_string(a->lua, "keybindings", keystr);
            if (!aname) aname = default_action_for(keystr);
            if (aname) {
                ActionFn fn = find_action(aname);
                if (fn) { fn(a); break; }
                /* Plugin-registered Lua action? */
                if (lua_host_invoke_action(a->lua, aname) == 0) break;
            }

            /* Edit-mode-only navigation / typing keys (not in the action
             * registry; they're tied to edit mode and not user-rebindable). */
            if (a->edit_mode) {
                switch (k) {
                    case SDLK_LEFT:      buffer_move_left (&a->buf, sel); break;
                    case SDLK_RIGHT:     buffer_move_right(&a->buf, sel); break;
                    case SDLK_UP:        buffer_move_up   (&a->buf, sel); break;
                    case SDLK_DOWN:      buffer_move_down (&a->buf, sel); break;
                    case SDLK_HOME:
                        if (ctrl) buffer_move_doc_start (&a->buf, sel);
                        else      buffer_move_line_start(&a->buf, sel);
                        break;
                    case SDLK_END:
                        if (ctrl) buffer_move_doc_end (&a->buf, sel);
                        else      buffer_move_line_end(&a->buf, sel);
                        break;
                    case SDLK_BACKSPACE: buffer_delete_back   (&a->buf); break;
                    case SDLK_DELETE:    buffer_delete_forward(&a->buf); break;
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER:
                        if (!smart_enter(a)) buffer_insert(&a->buf, "\n", 1);
                        break;
                    case SDLK_TAB:       buffer_insert(&a->buf, "    ", 4); break;
                    case SDLK_PAGEDOWN:  a->scroll_y += page_step; clamp_scroll(a); break;
                    case SDLK_PAGEUP:    a->scroll_y -= page_step; clamp_scroll(a); break;
                }
                update_window_title(a);
                ensure_cursor_visible(a);
                bump_blink(a);
                a->notification_until = 0;
            } else {
                /* Preview-mode scroll keys (also not user-rebindable yet). */
                if (k == SDLK_DOWN  || k == SDLK_j) a->scroll_y += line_px;
                if (k == SDLK_UP    || k == SDLK_k) a->scroll_y -= line_px;
                if (k == SDLK_PAGEDOWN)             a->scroll_y += page_step;
                if (k == SDLK_PAGEUP)               a->scroll_y -= page_step;
                if (k == SDLK_SPACE && (mod & KMOD_SHIFT)) a->scroll_y -= page_step;
                else if (k == SDLK_SPACE)                  a->scroll_y += page_step;
                if (k == SDLK_HOME) a->scroll_y = 0;
                if (k == SDLK_END)  a->scroll_y = max_scroll(a);
                clamp_scroll(a);
            }
            break;
        }
    }
}

int main(int argc, char** argv)
{
    /* Windows GUI-subsystem apps have no stderr — pipe it to a log file so
     * fprintf(stderr,...) diagnostics survive. Unbuffered so each line hits
     * disk immediately even on a hard kill. */
    if (!freopen("downsee.log", "w", stderr)) { /* nowhere to report */ }
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "downsee: log opened\n");

    App app = {0};
    const char* note = (argc > 1) ? argv[1] : NULL;
    if (app_init(&app, note) != 0) { app_shutdown(&app); return 1; }

    while (app.running) {
        SDL_Event e;
        /* When the renderer is mid-animation we need 60fps, not 20fps. */
        int wait_ms = app.wants_anim_frame ? 16 : 50;
        if (SDL_WaitEventTimeout(&e, wait_ms)) {
            do { app_event(&app, &e); } while (SDL_PollEvent(&e));
        }
        app_render(&app);
    }
    app_shutdown(&app);
    return 0;
}
