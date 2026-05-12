#ifndef DOWNSEE_FONT_H
#define DOWNSEE_FONT_H

#include <SDL.h>
#include <stddef.h>

typedef struct Font Font;

typedef enum {
    FONT_STYLE_REGULAR     = 0,
    FONT_STYLE_BOLD        = 1 << 0,
    FONT_STYLE_ITALIC      = 1 << 1,
    FONT_STYLE_BOLD_ITALIC = FONT_STYLE_BOLD | FONT_STYLE_ITALIC,
} FontStyle;

Font* font_create(SDL_Renderer* r, const char* ttf_path,
                  int pixel_size, FontStyle style);
void  font_destroy(Font* f);

/* Append a fallback font to the chain. Loaded at the same pixel size as the
 * primary, always FONT_STYLE_REGULAR (degrading bold/italic for missing
 * glyphs is preferable to tofu rectangles). */
void  font_add_fallback(Font* f, const char* ttf_path);

int   font_draw_line(Font* f, const char* utf8, size_t len,
                     int x_baseline, int y_baseline, SDL_Color color);

int   font_measure(Font* f, const char* utf8, size_t len);

int   font_line_height(const Font* f);
int   font_ascent     (const Font* f);
int   font_descent    (const Font* f);     /* >= 0; below baseline */

/* Print to stderr: for a small fixed set of diagnostic codepoints, which
 * font in the fallback chain claims to have the glyph (gi != 0). Useful
 * for debugging "this character tofus" complaints. */
void  font_diag(Font* f);

#endif
