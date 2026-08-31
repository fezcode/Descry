#ifndef DESCRY_ICONS_H
#define DESCRY_ICONS_H

#include <SDL.h>

/* SVG vector icons. nanosvg parses each SVG once; icon_raster.c then
 * supersamples + box-filters it to the exact device-pixel size on first
 * use and the SDL_Texture is cached per (icon, size). Textures are white
 * with alpha = coverage, so SDL_SetTextureColorMod tints the same texture
 * for every theme. */
typedef enum {
    ICON_SETTINGS,
    ICON_FIND,
    ICON_SIDEBAR_OPEN,
    ICON_SIDEBAR_CLOSED,
    ICON_OUTLINE,
    ICON_FOLDER,
    ICON_FOLDER_OPEN,
    ICON_FILE,
    ICON_CARET_RIGHT,
    ICON_CARET_DOWN,
    /* Window controls (custom title bar). Drawn on a 12-unit grid so the
     * 1-unit strokes land on whole pixels at the 12 px they are drawn at. */
    ICON_WIN_MIN,
    ICON_WIN_MAX,
    ICON_WIN_RESTORE,
    ICON_WIN_CLOSE,
    /* Settings adjuster chevrons. */
    ICON_CHEVRON_LEFT,
    ICON_CHEVRON_RIGHT,
    /* Vault-search (full-text across all notes). */
    ICON_VAULT_SEARCH,
    /* Command palette — terminal prompt glyph. */
    ICON_COMMAND,
    /* Split live-preview — panel split into two columns. */
    ICON_SPLIT,
    /* Plugins overlay — a plug. */
    ICON_PLUGIN,
    /* Filled variants, drawn for the selected / active state of the icon
     * above them (selected sidebar row, toggled toolbar button). */
    ICON_FILE_FILLED,
    ICON_FOLDER_FILLED,
    ICON_FOLDER_OPEN_FILLED,
    ICON_OUTLINE_FILLED,
    ICON_SPLIT_FILLED,
    ICON_PLUGIN_FILLED,
    ICON_COUNT,
} IconId;

int  icons_init    (SDL_Renderer* r);
void icons_shutdown(void);
void icon_draw     (SDL_Renderer* r, IconId id,
                    int x, int y, int sz, SDL_Color c);

/* Device pixels per logical point (1.0 on standard DPI, 2.0 on Retina).
 * Icons are rasterized at sz * scale device pixels and drawn into an
 * sz-point rect, so they stay crisp under SDL_RenderSetLogicalSize.
 * Changing the scale drops the texture cache. Safe to call before
 * icons_init. */
void icons_set_render_scale(float s);

/* Anti-aliased rounded rectangle ("pill"). Rasterized once per
 * (w, h, radius) via an analytic SDF, cached, and tinted via SDL color mod
 * when drawn. Replaces the stair-stepped fill_rrect for hero UI elements
 * (mode pill, badges) where corner smoothness shows. */
void pill_draw     (SDL_Renderer* r, int x, int y, int w, int h,
                    int radius, SDL_Color c);

#endif
