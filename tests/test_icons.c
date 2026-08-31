/* Unit tests for icon_raster.c — the pure (SDL-free) SVG -> RGBA pipeline
 * behind the UI icons: supersampled rasterization, box downsample,
 * knockout layer, un-premultiply, and the tiny-size minimum stroke. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nanosvg.h"
#include "icon_raster.h"

#define SVG_OPEN "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' " \
                 "fill='none' stroke='white' stroke-width='2'>"
#define SVG_CLOSE "</svg>"

static NSVGimage* parse(const char* src)
{
    char* copy = strdup(src);
    NSVGimage* img = nsvgParse(copy, "px", 96.0f);
    free(copy);
    assert(img);
    return img;
}

static unsigned char A(const unsigned char* px, int w, int x, int y)
{
    return px[(y * w + x) * 4 + 3];
}

static void test_supersample_factor(void)
{
    assert(icon_raster_supersample(64) == 4);
    assert(icon_raster_supersample(18) == 4);
    assert(icon_raster_supersample(16) == 4);
    assert(icon_raster_supersample(12) == 6);   /* ceil(64/12) */
    assert(icon_raster_supersample(7)  == 10);  /* ceil(64/7)  */
    assert(icon_raster_supersample(1)  >= 4);
}

static void test_downsample_averages_every_sample(void)
{
    /* 4x4 premultiplied source, ss=2 -> 2x2. Top-left block: two opaque
     * white + two transparent -> alpha 128 (255*2/4 rounded). */
    unsigned char src[4 * 4 * 4];
    memset(src, 0, sizeof src);
    int opaque[][2] = { {0,0}, {1,0},           /* block (0,0): 2 of 4 */
                        {2,0}, {3,0}, {2,1}, {3,1}, /* block (1,0): 4 of 4 */
                        {0,2} };                /* block (0,1): 1 of 4 */
    for (size_t i = 0; i < sizeof opaque / sizeof opaque[0]; ++i) {
        int o = (opaque[i][1] * 4 + opaque[i][0]) * 4;
        src[o] = src[o+1] = src[o+2] = src[o+3] = 255;
    }
    unsigned char dst[2 * 2 * 4];
    icon_raster_downsample(src, 2, 2, 2, dst);
    assert(A(dst, 2, 0, 0) == 128);
    assert(A(dst, 2, 1, 0) == 255);
    assert(A(dst, 2, 0, 1) == 64);
    assert(A(dst, 2, 1, 1) == 0);
    /* Premultiplied colour averages the same way. */
    assert(dst[0] == 128 && dst[1] == 128 && dst[2] == 128);
}

static void test_cut_knocks_out(void)
{
    unsigned char dst[3 * 4] = { 255,255,255,255,  255,255,255,255,  200,200,200,200 };
    unsigned char cut[3 * 4] = { 0,0,0,255,        0,0,0,0,          0,0,0,128 };
    icon_raster_cut(dst, cut, 3, 1);
    assert(dst[3]  == 0 && dst[0] == 0);            /* fully cut       */
    assert(dst[7]  == 255 && dst[4] == 255);        /* untouched       */
    assert(dst[11] >= 98 && dst[11] <= 101);        /* 200 * 127/255   */
    assert(dst[8]  == dst[11]);                     /* stays premult.  */
}

static void test_unpremultiply(void)
{
    unsigned char px[2 * 4] = { 128,128,128,128,  0,0,0,0 };
    icon_raster_unpremultiply(px, 2);
    assert(px[0] == 255 && px[1] == 255 && px[2] == 255 && px[3] == 128);
    assert(px[4] == 0 && px[7] == 0);
}

static void test_raster_edges_are_antialiased(void)
{
    /* A rect whose left edge sits at x=4.5 viewBox units: at 24px it
     * half-covers pixel column 4 -> ~50% alpha there, 0 at column 3,
     * 255 at column 5. That is what a proper supersample must produce
     * (nearest/bilinear-minified textures give 0 or 255 here). */
    NSVGimage* img = parse(SVG_OPEN
        "<rect x='4.5' y='4' width='15.5' height='16' fill='white' stroke='none'/>"
        SVG_CLOSE);
    unsigned char* px = icon_raster(img, NULL, 24, 0.0f);
    assert(px);
    assert(A(px, 24, 3, 12) == 0);
    assert(A(px, 24, 4, 12) >= 112 && A(px, 24, 4, 12) <= 144);
    assert(A(px, 24, 5, 12) == 255);
    assert(A(px, 24, 12, 12) == 255);
    assert(A(px, 24, 12, 3) == 0);
    /* Straight alpha: colour stays white where partially covered. */
    assert(px[(12 * 24 + 4) * 4 + 0] == 255);
    free(px);

    /* Same SVG at 12px: the rect scales to 2.25..10, so column 2 is 75%. */
    px = icon_raster(img, NULL, 12, 0.0f);
    assert(px);
    assert(A(px, 12, 1, 6) == 0);
    assert(A(px, 12, 2, 6) >= 176 && A(px, 12, 2, 6) <= 208);
    assert(A(px, 12, 6, 6) == 255);
    free(px);
    nsvgDelete(img);
}

static void test_raster_cut_layer(void)
{
    NSVGimage* img = parse(SVG_OPEN
        "<rect x='2' y='2' width='20' height='20' fill='white' stroke='none'/>"
        SVG_CLOSE);
    NSVGimage* cut = parse(SVG_OPEN
        "<line x1='12' y1='0' x2='12' y2='24' stroke-width='4'/>"
        SVG_CLOSE);
    unsigned char* px = icon_raster(img, cut, 24, 0.0f);
    assert(px);
    assert(A(px, 24, 5, 12)  == 255);   /* body            */
    assert(A(px, 24, 10, 12) == 0);     /* slit 10..14     */
    assert(A(px, 24, 13, 12) == 0);
    assert(A(px, 24, 14, 12) == 255);
    assert(A(px, 24, 1, 12)  == 0);     /* outside body    */
    free(px);
    nsvgDelete(img);
    nsvgDelete(cut);
}

static void test_min_stroke_thickens_tiny_lines(void)
{
    /* 1-unit horizontal line at 6px is 0.25px thick -> ~64 alpha spread
     * over rows 2/3. With a 1px floor the two rows should sum to ~255. */
    NSVGimage* img = parse(SVG_OPEN
        "<line x1='0' y1='12' x2='24' y2='12' stroke-width='1'/>"
        SVG_CLOSE);
    unsigned char* thin = icon_raster(img, NULL, 6, 0.0f);
    unsigned char* fat  = icon_raster(img, NULL, 6, 1.0f);
    assert(thin && fat);
    int thin_sum = A(thin, 6, 3, 2) + A(thin, 6, 3, 3);
    int fat_sum  = A(fat,  6, 3, 2) + A(fat,  6, 3, 3);
    assert(thin_sum < 100);
    assert(fat_sum >= 230 && fat_sum <= 280);
    assert(A(fat, 6, 3, 0) == 0 && A(fat, 6, 3, 5) == 0);
    /* The parsed image must come back untouched. */
    assert(img->shapes && img->shapes->strokeWidth == 1.0f);
    free(thin);
    free(fat);
    nsvgDelete(img);
}

int main(void)
{
    test_supersample_factor();
    test_downsample_averages_every_sample();
    test_cut_knocks_out();
    test_unpremultiply();
    test_raster_edges_are_antialiased();
    test_raster_cut_layer();
    test_min_stroke_thickens_tiny_lines();
    printf("test_icons: all passed\n");
    return 0;
}
