#ifndef DESCRY_ICON_RASTER_H
#define DESCRY_ICON_RASTER_H

/* Pure (no SDL) SVG -> straight-alpha RGBA rasterization for UI icons.
 *
 * nanosvg's rasterizer anti-aliases only modestly (5 vertical subsamples),
 * and a 24-unit icon drawn at 16 px has 1.3 px strokes it cannot resolve
 * cleanly. Handing the GPU an oversampled texture to bilinear-minify does
 * not help either: a 2x2 bilinear tap over a 3x texture skips more than
 * half the samples, which is exactly the stair-stepping this module
 * replaces. So: rasterize at N x the target size, box-filter EVERY sample
 * down in software, and return a buffer meant to be blitted 1:1. */

typedef struct NSVGimage NSVGimage;

/* Supersample factor for a target size in device pixels: 4 for normal
 * sizes, more for tiny glyphs so the working raster never drops below
 * ~64 px on a side. */
int icon_raster_supersample(int px);

/* Box-filter a premultiplied RGBA buffer of (w*ss) x (h*ss) pixels down to
 * w x h. Every source sample contributes equally; rounds to nearest. */
void icon_raster_downsample(const unsigned char* src, int w, int h, int ss,
                            unsigned char* dst);

/* Knockout: multiply every channel of premultiplied `dst` by
 * (1 - cut.alpha). `cut` is a same-sized premultiplied RGBA buffer. */
void icon_raster_cut(unsigned char* dst, const unsigned char* cut, int w, int h);

/* Premultiplied -> straight alpha, in place, n pixels. */
void icon_raster_unpremultiply(unsigned char* px, int n);

/* Full pipeline. Rasterizes `img` (and subtracts `cut`, if non-NULL) at
 * px x px device pixels. Strokes thinner than `min_stroke_px` device
 * pixels are widened to that floor while rasterizing (the parsed image is
 * left as it was); pass 0 to disable. Icons are monochrome: the result is
 * white with alpha = coverage, so a single texture tints to any colour.
 * Returns a malloc'd px*px*4 straight-alpha buffer in R,G,B,A byte order,
 * or NULL on failure. */
unsigned char* icon_raster(NSVGimage* img, NSVGimage* cut, int px,
                           float min_stroke_px);

/* Free the module's scratch rasterizer (safe to call without a prior
 * icon_raster). */
void icon_raster_shutdown(void);

#endif
