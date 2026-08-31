/* See icon_raster.h. This is the one translation unit that carries the
 * nanosvg implementations; icons.c only parses through the header API. */
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast.h"

#include "icon_raster.h"

#include <stdlib.h>
#include <string.h>

static NSVGrasterizer* g_rast;

int icon_raster_supersample(int px)
{
    if (px < 1) px = 1;
    int ss = (64 + px - 1) / px;
    return ss < 4 ? 4 : ss;
}

void icon_raster_downsample(const unsigned char* src, int w, int h, int ss,
                            unsigned char* dst)
{
    const size_t sw = (size_t)w * ss;
    const unsigned n = (unsigned)(ss * ss);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned acc[4] = { 0, 0, 0, 0 };
            for (int sy = 0; sy < ss; ++sy) {
                const unsigned char* p =
                    src + (((size_t)y * ss + sy) * sw + (size_t)x * ss) * 4;
                for (int sx = 0; sx < ss; ++sx, p += 4) {
                    acc[0] += p[0]; acc[1] += p[1];
                    acc[2] += p[2]; acc[3] += p[3];
                }
            }
            unsigned char* o = dst + ((size_t)y * w + x) * 4;
            for (int c = 0; c < 4; ++c)
                o[c] = (unsigned char)((acc[c] + n / 2) / n);
        }
    }
}

void icon_raster_cut(unsigned char* dst, const unsigned char* cut, int w, int h)
{
    const size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; ++i) {
        unsigned keep = 255u - cut[i * 4 + 3];
        if (keep == 255u) continue;
        for (int c = 0; c < 4; ++c)
            dst[i * 4 + c] = (unsigned char)((dst[i * 4 + c] * keep + 127u) / 255u);
    }
}

void icon_raster_unpremultiply(unsigned char* px, int n)
{
    for (int i = 0; i < n; ++i) {
        unsigned a = px[i * 4 + 3];
        if (a == 0 || a == 255) continue;
        for (int c = 0; c < 3; ++c) {
            unsigned v = (px[i * 4 + c] * 255u + a / 2) / a;
            px[i * 4 + c] = (unsigned char)(v > 255u ? 255u : v);
        }
    }
}

/* Rasterize one layer at `scale` (raster px per viewBox unit) into a
 * W x W premultiplied buffer. Strokes are floored to `min_stroke` raster
 * pixels for the duration of the call and restored afterwards, so the
 * cached parse stays pristine for other sizes. */
static void raster_layer(NSVGimage* img, unsigned char* dst, int W,
                         float scale, float min_stroke)
{
    int n = 0;
    for (NSVGshape* s = img->shapes; s; s = s->next) ++n;
    float* saved = n ? (float*)malloc((size_t)n * sizeof *saved) : NULL;
    int i = 0;
    for (NSVGshape* s = img->shapes; s; s = s->next, ++i) {
        if (saved) saved[i] = s->strokeWidth;
        if (saved && min_stroke > 0.0f && s->stroke.type != NSVG_PAINT_NONE &&
            s->strokeWidth * scale < min_stroke)
            s->strokeWidth = min_stroke / scale;
    }
    nsvgRasterize(g_rast, img, 0, 0, scale, dst, W, W, W * 4);
    i = 0;
    for (NSVGshape* s = img->shapes; s; s = s->next, ++i)
        if (saved) s->strokeWidth = saved[i];
    free(saved);
}

static float fit_scale(const NSVGimage* img, int W)
{
    float big = img->width > img->height ? img->width : img->height;
    return (float)W / big;
}

unsigned char* icon_raster(NSVGimage* img, NSVGimage* cut, int px,
                           float min_stroke_px)
{
    if (!img || px <= 0 || img->width <= 0 || img->height <= 0) return NULL;
    if (!g_rast) g_rast = nsvgCreateRasterizer();
    if (!g_rast) return NULL;

    const int ss = icon_raster_supersample(px);
    const int W  = px * ss;
    const size_t big = (size_t)W * W * 4;
    const float min_stroke = min_stroke_px * (float)ss;

    unsigned char* hi = (unsigned char*)calloc(big, 1);
    if (!hi) return NULL;
    raster_layer(img, hi, W, fit_scale(img, W), min_stroke);

    if (cut && cut->width > 0 && cut->height > 0) {
        unsigned char* hc = (unsigned char*)calloc(big, 1);
        if (hc) {
            raster_layer(cut, hc, W, fit_scale(cut, W), min_stroke);
            icon_raster_cut(hi, hc, W, W);
            free(hc);
        }
    }

    unsigned char* out = (unsigned char*)malloc((size_t)px * px * 4);
    if (!out) { free(hi); return NULL; }
    icon_raster_downsample(hi, px, px, ss, out);
    free(hi);

    icon_raster_unpremultiply(out, px * px);
    /* Monochrome by contract: force white so fully transparent texels do
     * not bleed black into any filtered sample on fractional-DPI mappings. */
    for (int i = 0; i < px * px; ++i) {
        out[i * 4 + 0] = 255;
        out[i * 4 + 1] = 255;
        out[i * 4 + 2] = 255;
    }
    return out;
}

void icon_raster_shutdown(void)
{
    if (g_rast) nsvgDeleteRasterizer(g_rast);
    g_rast = NULL;
}
