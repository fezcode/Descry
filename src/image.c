#include "image.h"

#include <png.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ImageEntry {
    char*               path;
    SDL_Texture*        tex;
    int                 w, h;
    int                 failed;
    struct ImageEntry*  next;
} ImageEntry;

struct ImageCache {
    SDL_Renderer* renderer;
    ImageEntry*   head;
};

ImageCache* image_cache_create(SDL_Renderer* r)
{
    ImageCache* c = calloc(1, sizeof *c);
    c->renderer = r;
    return c;
}

void image_cache_destroy(ImageCache* c)
{
    if (!c) return;
    for (ImageEntry* e = c->head; e; ) {
        ImageEntry* n = e->next;
        free(e->path);
        if (e->tex) SDL_DestroyTexture(e->tex);
        free(e);
        e = n;
    }
    free(c);
}

static unsigned char* load_png(const char* path, int* w, int* h)
{
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    png_byte header[8];
    if (fread(header, 1, 8, fp) != 8 ||
        png_sig_cmp(header, 0, 8) != 0) { fclose(fp); return NULL; }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return NULL; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(fp); return NULL; }

    unsigned char* pixels = NULL;
    png_bytep* rows = NULL;
    if (setjmp(png_jmpbuf(png))) {
        free(pixels); free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return NULL;
    }

    png_init_io(png, fp);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    *w = (int)png_get_image_width(png, info);
    *h = (int)png_get_image_height(png, info);

    int color_type = png_get_color_type(png, info);
    int bit_depth  = png_get_bit_depth(png, info);

    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);

    png_read_update_info(png, info);
    int row_size = (int)png_get_rowbytes(png, info);
    pixels = malloc((size_t)(*h) * row_size);
    rows   = malloc((size_t)(*h) * sizeof(png_bytep));
    for (int y = 0; y < *h; ++y) rows[y] = pixels + (size_t)y * row_size;
    png_read_image(png, rows);
    free(rows); rows = NULL;

    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return pixels;
}

struct jpeg_err_jmp {
    struct jpeg_error_mgr pub;
    jmp_buf               jmp;
};

static void jpeg_err_exit(j_common_ptr cinfo)
{
    longjmp(((struct jpeg_err_jmp*)cinfo->err)->jmp, 1);
}

static unsigned char* load_jpg(const char* path, int* w, int* h)
{
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;

    struct jpeg_decompress_struct cinfo;
    struct jpeg_err_jmp           err;
    unsigned char*                pixels = NULL;
    JSAMPLE*                      row    = NULL;

    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = jpeg_err_exit;
    if (setjmp(err.jmp)) {
        free(pixels); free(row);
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return NULL;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);

    *w = (int)cinfo.output_width;
    *h = (int)cinfo.output_height;
    int comps = cinfo.output_components;

    pixels = malloc((size_t)(*w) * (*h) * 4);
    row    = malloc((size_t)(*w) * comps);

    int y = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, &row, 1);
        unsigned char* dst = pixels + (size_t)y * (*w) * 4;
        for (int x = 0; x < *w; ++x) {
            dst[x*4 + 0] = row[x*comps + 0];
            dst[x*4 + 1] = comps >= 2 ? row[x*comps + 1] : row[x*comps + 0];
            dst[x*4 + 2] = comps >= 3 ? row[x*comps + 2] : row[x*comps + 0];
            dst[x*4 + 3] = 0xFF;
        }
        y++;
    }
    free(row); row = NULL;

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    return pixels;
}

static SDL_Texture* upload(SDL_Renderer* r, unsigned char* px, int w, int h)
{
    /* RGBA byte order in our buffer matches SDL_PIXELFORMAT_ABGR8888 on
     * little-endian. (Byte 0 = R, 1 = G, 2 = B, 3 = A.) */
    SDL_Texture* tex = SDL_CreateTexture(r,
        SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!tex) return NULL;
    SDL_UpdateTexture(tex, NULL, px, w * 4);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}

static int has_ext(const char* path, const char* ext)
{
    size_t pn = strlen(path), en = strlen(ext);
    if (pn < en) return 0;
    for (size_t i = 0; i < en; ++i) {
        char p = path[pn - en + i], e = ext[i];
        if (p >= 'A' && p <= 'Z') p = (char)(p - 'A' + 'a');
        if (p != e) return 0;
    }
    return 1;
}

SDL_Texture* image_cache_get(ImageCache* c, const char* path,
                             int* w, int* h)
{
    for (ImageEntry* e = c->head; e; e = e->next) {
        if (strcmp(e->path, path) == 0) {
            if (e->failed) return NULL;
            *w = e->w; *h = e->h;
            return e->tex;
        }
    }

    unsigned char* pixels = NULL;
    int iw = 0, ih = 0;
    if      (has_ext(path, ".png"))                            pixels = load_png(path, &iw, &ih);
    else if (has_ext(path, ".jpg") || has_ext(path, ".jpeg"))  pixels = load_jpg(path, &iw, &ih);
    /* TODO: webp, gif, bmp */

    SDL_Texture* tex = pixels ? upload(c->renderer, pixels, iw, ih) : NULL;
    free(pixels);

    ImageEntry* e = calloc(1, sizeof *e);
    e->path   = strdup(path);
    e->tex    = tex;
    e->w      = iw;
    e->h      = ih;
    e->failed = (tex == NULL);
    e->next   = c->head;
    c->head   = e;

    if (tex) { *w = iw; *h = ih; return tex; }
    return NULL;
}
