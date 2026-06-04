#ifndef DESCRY_ICONS_H
#define DESCRY_ICONS_H

#include <SDL.h>

/* SVG-rasterized vector icons. nanosvg parses each SVG once, then we
 * lazily rasterize+cache an SDL_Texture per (icon, size). Color is applied
 * via SDL_SetTextureColorMod so the same texture serves every theme. */
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
    /* Window controls (custom title bar). */
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
    ICON_COUNT,
} IconId;

int  icons_init    (SDL_Renderer* r);
void icons_shutdown(void);
void icon_draw     (SDL_Renderer* r, IconId id,
                    int x, int y, int sz, SDL_Color c);

/* Anti-aliased rounded rectangle ("pill"). Rasterized once per
 * (w, h, radius) via nanosvg, cached, and tinted via SDL color mod when
 * drawn. Replaces the stair-stepped fill_rrect for hero UI elements
 * (mode pill, badges) where corner smoothness shows. */
void pill_draw     (SDL_Renderer* r, int x, int y, int w, int h,
                    int radius, SDL_Color c);

#endif
