#ifndef DESCRY_SVG_EXTRA_H
#define DESCRY_SVG_EXTRA_H

#include <stddef.h>

/* The parts of SVG that nanosvg drops on the floor.
 *
 * nanosvg is a shape rasterizer: paths, rects, circles, gradients. It has no
 * handler at all for <text>, <image> or <clipPath>, and silently ignores
 * them. That is fine for the icon set (hand-authored paths) but it is most of
 * a README badge -- a shields.io badge is two <rect>s, two or three <text>
 * labels, an optional logo <image> holding a nested SVG data URI, and a
 * <clipPath> with one rounded rect that gives the badge its corners. Rendered
 * through nanosvg alone a badge comes out as two square colour bars and
 * nothing else.
 *
 * So: parse the source a second time for those three element kinds, then
 * composite them on top of the buffer nanosvg produced. The parser
 * (svg_extra_parse) is pure -- no SDL, no FreeType, no nanosvg -- so it can be
 * unit-tested on a source string; the renderer (svg_extra_render) needs
 * FreeType + HarfBuzz for glyphs and nanosvg for nested SVG logos.
 *
 * Known limits, all of them deliberate:
 *   - Extras are composited ON TOP of every nanosvg shape rather than in
 *     document order. Badges (and mermaid-style diagrams) draw their text
 *     last anyway.
 *   - <tspan> contributes its characters but not its own x/y/fill; a tspan
 *     inherits the enclosing <text>'s style.
 *   - Text is placed through the full transform chain but glyphs are not
 *     rotated or skewed: only the scale component reaches the glyph raster.
 *   - <image> handles data:image/svg+xml URIs (base64 or percent-encoded),
 *     which is what badge generators emit for named logos. Raster data URIs
 *     and external hrefs are skipped.
 *   - The clip rect is applied only when it covers the whole canvas, so a
 *     clipPath used on one small sub-element can never erase the rest of the
 *     image. That is precisely the badge "round the corners" case.
 */

typedef enum {
    SVG_ANCHOR_START = 0,
    SVG_ANCHOR_MIDDLE,
    SVG_ANCHOR_END,
} SvgAnchor;

typedef struct {
    char*         text;      /* UTF-8, entities decoded, whitespace collapsed */
    float         x, y;      /* baseline anchor, user units, transform applied */
    float         size;      /* font size in user units, transform applied     */
    float         length;    /* textLength, user units; 0 = natural advance    */
    SvgAnchor     anchor;
    int           bold, italic;
    char          family[96];/* the raw font-family list, resolved at render   */
    unsigned char r, g, b;   /* fill colour                                    */
    float         opacity;   /* fill-opacity folded with the opacity chain     */
} SvgTextRun;

typedef struct {
    float x, y, w, h;        /* user units, transform applied */
    char* href;              /* owned; a data: URI             */
} SvgImageRef;

typedef struct {
    int   present;           /* a clipPath with one rect exists AND is used */
    float x, y, w, h;        /* user units */
    float rx, ry;
} SvgClipRect;

typedef struct SvgExtra SvgExtra;

/* Parse `len` bytes of SVG source. Never modifies `src`. Returns NULL only on
 * allocation failure; a document with no extras parses to a non-NULL handle
 * for which svg_extra_empty() is true. */
SvgExtra* svg_extra_parse(const char* src, size_t len);
void      svg_extra_free (SvgExtra* e);

int                svg_extra_empty      (const SvgExtra* e);
int                svg_extra_text_count (const SvgExtra* e);
const SvgTextRun*  svg_extra_text       (const SvgExtra* e, int i);
int                svg_extra_image_count(const SvgExtra* e);
const SvgImageRef* svg_extra_image      (const SvgExtra* e, int i);
const SvgClipRect* svg_extra_clip       (const SvgExtra* e);

/* Composite the extras onto `rgba`, a straight-alpha w x h RGBA buffer that
 * was rasterized at `scale` device pixels per SVG user unit. Main thread only
 * (it owns a process-wide FreeType face cache). */
void svg_extra_render(const SvgExtra* e, unsigned char* rgba,
                      int w, int h, float scale);

/* Last-resort font for <text> whose family matches nothing this platform
 * ships -- the app points it at the configured UI font. Copied, not aliased. */
void svg_extra_set_fallback_font(const char* ttf_path);

/* Release the cached FreeType faces and library. */
void svg_extra_shutdown(void);

#endif
