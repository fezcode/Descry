#include "image.h"

#include <png.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* nanosvg is header-only; the IMPLEMENTATION lives in icons.c, so here we
 * include the headers WITHOUT the impl macros — declarations only, no
 * duplicate symbols at link time. */
#include "nanosvg.h"
#include "nanosvgrast.h"

#if defined(_WIN32)
  #include <windows.h>
  #include <wininet.h>
#endif

/* ----------------------------------------------------------------------- *
 * Cache entry / job model
 *
 * Entries are append-only until image_cache_destroy, so a pointer handed to
 * the download worker stays valid for the worker's lifetime. Only the main
 * (render) thread traverses/extends the entry list; the worker touches just
 * the single entry carried by its job, and communicates back through the
 * atomic `state`. SDL textures are created exclusively on the main thread.
 * ----------------------------------------------------------------------- */

typedef enum {
    ST_READY,        /* tex valid */
    ST_LOADING,      /* URL fetch in flight (or queued) */
    ST_DOWNLOADED,   /* bytes on disk at temp_path, awaiting main-thread decode */
    ST_FAILED,       /* gave up */
} EntryState;

typedef struct ImageEntry {
    char*               key;        /* original src: local path or URL */
    char*               temp_path;  /* URL only: on-disk cache file */
    SDL_Texture*        tex;
    int                 w, h;
    SDL_atomic_t        state;      /* EntryState, shared with worker */
    int                 is_url;
    struct ImageEntry*  next;
} ImageEntry;

typedef struct Job {
    ImageEntry*  entry;             /* stable pointer (never freed pre-destroy) */
    char*        url;
    char*        temp_path;
    struct Job*  next;
} Job;

struct ImageCache {
    SDL_Renderer* renderer;
    ImageEntry*   head;

    SDL_Thread*   worker;
    SDL_mutex*    lock;
    SDL_cond*     cond;
    Job*          jobs_head;
    Job*          jobs_tail;
    int           shutting_down;
    Uint32        wake_event;       /* user event type used to wake the loop */
};

/* ----------------------------------------------------------------------- *
 * PNG / JPEG decoders (unchanged) — return malloc'd RGBA (R,G,B,A bytes).
 * ----------------------------------------------------------------------- */

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

/* SVG → malloc'd RGBA. nanosvg renders vector shapes only: it ignores
 * <script>, CSS, and external references, so there's no remote code
 * execution or hidden network fetch from rasterizing untrusted SVG. */
static unsigned char* load_svg(const char* path, int* w, int* h)
{
    /* nsvgParseFromFile mutates a private copy it reads itself. */
    NSVGimage* img = nsvgParseFromFile(path, "px", 96.0f);
    if (!img) return NULL;

    int iw = (int)(img->width  + 0.5f);
    int ih = (int)(img->height + 0.5f);
    /* Some SVGs declare only a viewBox and no width/height → nanosvg leaves
     * width/height 0. Fall back to a sane default canvas. */
    if (iw <= 0 || ih <= 0) { iw = 512; ih = 512; }
    /* Clamp absurd sizes so a malformed viewBox can't ask for gigabytes. */
    const int MAXDIM = 4096;
    float scale = 1.0f;
    if (iw > MAXDIM || ih > MAXDIM) {
        scale = (float)MAXDIM / (iw > ih ? iw : ih);
        iw = (int)(iw * scale);
        ih = (int)(ih * scale);
        if (iw <= 0) iw = 1;
        if (ih <= 0) ih = 1;
    }

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(img); return NULL; }

    unsigned char* px = calloc((size_t)iw * ih * 4, 1);
    if (!px) { nsvgDeleteRasterizer(rast); nsvgDelete(img); return NULL; }

    nsvgRasterize(rast, img, 0, 0, scale, px, iw, ih, iw * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);

    /* nanosvg outputs PREMULTIPLIED RGBA; SDL_BLENDMODE_BLEND wants straight
     * alpha, so un-premultiply (mirrors icons.c). */
    for (int i = 0; i < iw * ih; ++i) {
        unsigned char a = px[i*4 + 3];
        if (a == 0 || a == 255) continue;
        px[i*4 + 0] = (unsigned char)(px[i*4 + 0] * 255 / a);
        px[i*4 + 1] = (unsigned char)(px[i*4 + 1] * 255 / a);
        px[i*4 + 2] = (unsigned char)(px[i*4 + 2] * 255 / a);
    }

    *w = iw; *h = ih;
    return px;
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

/* ----------------------------------------------------------------------- *
 * Format detection (by extension, falling back to a content sniff so a URL
 * with no/wrong extension still decodes).
 * ----------------------------------------------------------------------- */

typedef enum { FMT_UNKNOWN, FMT_PNG, FMT_JPG, FMT_SVG } ImgFmt;

static ImgFmt fmt_from_ext(const char* path)
{
    if (has_ext(path, ".png"))                          return FMT_PNG;
    if (has_ext(path, ".jpg") || has_ext(path, ".jpeg")) return FMT_JPG;
    if (has_ext(path, ".svg"))                          return FMT_SVG;
    return FMT_UNKNOWN;
}

static ImgFmt fmt_sniff(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return FMT_UNKNOWN;
    unsigned char b[256];
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    if (n >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G')
        return FMT_PNG;
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
        return FMT_JPG;
    /* SVG: an "<svg" tag within the first chunk (skips any XML prolog / BOM). */
    for (size_t i = 0; i + 4 <= n; ++i) {
        if (b[i] == '<' &&
            (b[i+1] == 's' || b[i+1] == 'S') &&
            (b[i+2] == 'v' || b[i+2] == 'V') &&
            (b[i+3] == 'g' || b[i+3] == 'G'))
            return FMT_SVG;
    }
    return FMT_UNKNOWN;
}

static ImgFmt fmt_of(const char* path)
{
    ImgFmt f = fmt_from_ext(path);
    return f != FMT_UNKNOWN ? f : fmt_sniff(path);
}

static SDL_Texture* decode_file(SDL_Renderer* r, const char* path, int* w, int* h)
{
    unsigned char* px = NULL;
    int iw = 0, ih = 0;
    switch (fmt_of(path)) {
        case FMT_PNG: px = load_png(path, &iw, &ih); break;
        case FMT_JPG: px = load_jpg(path, &iw, &ih); break;
        case FMT_SVG: px = load_svg(path, &iw, &ih); break;
        default: break;   /* gif/webp/bmp/unknown: unsupported for now */
    }
    SDL_Texture* tex = px ? upload(r, px, iw, ih) : NULL;
    free(px);
    if (tex) { *w = iw; *h = ih; }
    return tex;
}

/* ----------------------------------------------------------------------- *
 * URL helpers + disk cache pathing
 * ----------------------------------------------------------------------- */

static int is_http_url(const char* s)
{
    return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0;
}

static const char* temp_dir(void)
{
#if defined(_WIN32)
    const char* t = getenv("TEMP");
    if (!t || !t[0]) t = getenv("TMP");
    if (!t || !t[0]) t = ".";
    return t;
#else
    const char* t = getenv("TMPDIR");
    if (!t || !t[0]) t = "/tmp";
    return t;
#endif
}

static void ensure_cache_dir(char* out, size_t cap)
{
    const char* t = temp_dir();
#if defined(_WIN32)
    snprintf(out, cap, "%s\\downsee_cache", t);
    CreateDirectoryA(out, NULL);
#else
    snprintf(out, cap, "%s/downsee_cache", t);
    mkdir(out, 0755);
#endif
}

static unsigned long long fnv1a(const char* s)
{
    unsigned long long h = 1469598103934665603ULL;
    for (; *s; ++s) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

/* Lowercased known-image extension from the URL path (ignoring ?query/#frag),
 * or "" if none recognized (decoder will sniff). */
static void url_ext(const char* url, char* out, size_t cap)
{
    out[0] = 0;
    const char* end = url + strlen(url);
    for (const char* p = url; p < end; ++p)
        if (*p == '?' || *p == '#') { end = p; break; }
    const char* dot = NULL; const char* slash = NULL;
    for (const char* p = url; p < end; ++p) {
        if (*p == '.') dot = p;
        else if (*p == '/') slash = p;
    }
    if (dot && (!slash || dot > slash)) {
        size_t n = (size_t)(end - dot);
        if (n > 0 && n < cap) {
            for (size_t i = 0; i < n; ++i) {
                char c = dot[i];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                out[i] = c;
            }
            out[n] = 0;
        }
    }
    if (!(strcmp(out, ".png") == 0 || strcmp(out, ".jpg") == 0 ||
          strcmp(out, ".jpeg") == 0 || strcmp(out, ".svg") == 0))
        out[0] = 0;
}

static char* build_cache_path(const char* url)
{
    char dir[1024];
    ensure_cache_dir(dir, sizeof dir);
    char ext[16];
    url_ext(url, ext, sizeof ext);
    unsigned long long h = fnv1a(url);
    char path[1280];
#if defined(_WIN32)
    snprintf(path, sizeof path, "%s\\%016llx%s", dir, h, ext);
#else
    snprintf(path, sizeof path, "%s/%016llx%s", dir, h, ext);
#endif
    return strdup(path);
}

static int file_exists(const char* p)
{
    FILE* f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* Download `url` to `out_path` (via a .part temp + atomic rename, so a file
 * that exists is always complete). GET only — no script execution, ever.
 * Returns 0 on success. Without WinINet (non-Windows) remote images are
 * unsupported and this always fails. */
static int download_url(const char* url, const char* out_path)
{
#if defined(_WIN32)
    HINTERNET hi = InternetOpenA("Downsee/1.0", INTERNET_OPEN_TYPE_PRECONFIG,
                                 NULL, NULL, 0);
    if (!hi) return -1;

    DWORD timeout = 10000;   /* 10s connect + receive */
    InternetSetOptionA(hi, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof timeout);
    InternetSetOptionA(hi, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof timeout);

    DWORD flags = INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_CACHE_WRITE |
                  INTERNET_FLAG_RELOAD;
    HINTERNET h = InternetOpenUrlA(hi, url, NULL, 0, flags, 0);
    if (!h) { InternetCloseHandle(hi); return -1; }

    char part[1280];
    snprintf(part, sizeof part, "%s.part", out_path);
    FILE* f = fopen(part, "wb");
    if (!f) { InternetCloseHandle(h); InternetCloseHandle(hi); return -1; }

    const size_t MAX_BYTES = 25u * 1024 * 1024;   /* 25 MB cap */
    char buf[16384];
    DWORD got = 0;
    size_t total = 0;
    int ok = 1;
    while (InternetReadFile(h, buf, sizeof buf, &got) && got > 0) {
        total += got;
        if (total > MAX_BYTES)            { ok = 0; break; }
        if (fwrite(buf, 1, got, f) != got){ ok = 0; break; }
    }
    fclose(f);
    InternetCloseHandle(h);
    InternetCloseHandle(hi);

    if (!ok || total == 0) { remove(part); return -1; }
    remove(out_path);                       /* clear any stale file */
    if (rename(part, out_path) != 0) { remove(part); return -1; }
    return 0;
#else
    (void)url; (void)out_path;
    return -1;
#endif
}

/* ----------------------------------------------------------------------- *
 * Background download worker
 * ----------------------------------------------------------------------- */

static int worker_main(void* ud)
{
    ImageCache* c = (ImageCache*)ud;
    for (;;) {
        SDL_LockMutex(c->lock);
        while (!c->shutting_down && !c->jobs_head)
            SDL_CondWait(c->cond, c->lock);
        if (c->shutting_down) { SDL_UnlockMutex(c->lock); break; }
        Job* j = c->jobs_head;
        c->jobs_head = j->next;
        if (!c->jobs_head) c->jobs_tail = NULL;
        SDL_UnlockMutex(c->lock);

        int rc = download_url(j->url, j->temp_path);
        SDL_AtomicSet(&j->entry->state, rc == 0 ? ST_DOWNLOADED : ST_FAILED);

        /* Wake the (possibly idle) main loop so it re-renders and decodes. */
        if (c->wake_event != (Uint32)-1) {
            SDL_Event ev;
            SDL_zero(ev);
            ev.type = c->wake_event;
            SDL_PushEvent(&ev);
        }

        free(j->url); free(j->temp_path); free(j);
    }
    return 0;
}

static void enqueue_job(ImageCache* c, ImageEntry* e,
                        const char* url, const char* temp)
{
    Job* j = calloc(1, sizeof *j);
    j->entry     = e;
    j->url       = strdup(url);
    j->temp_path = strdup(temp);
    SDL_LockMutex(c->lock);
    if (c->jobs_tail) c->jobs_tail->next = j; else c->jobs_head = j;
    c->jobs_tail = j;
    SDL_CondSignal(c->cond);
    SDL_UnlockMutex(c->lock);
}

/* ----------------------------------------------------------------------- *
 * Public API
 * ----------------------------------------------------------------------- */

ImageCache* image_cache_create(SDL_Renderer* r)
{
    ImageCache* c = calloc(1, sizeof *c);
    c->renderer   = r;
    c->lock       = SDL_CreateMutex();
    c->cond       = SDL_CreateCond();
    c->wake_event = SDL_RegisterEvents(1);   /* (Uint32)-1 if unavailable */
    c->worker     = SDL_CreateThread(worker_main, "downsee-img", c);
    return c;
}

void image_cache_destroy(ImageCache* c)
{
    if (!c) return;
    if (c->worker) {
        SDL_LockMutex(c->lock);
        c->shutting_down = 1;
        SDL_CondSignal(c->cond);
        SDL_UnlockMutex(c->lock);
        SDL_WaitThread(c->worker, NULL);
    }
    for (Job* j = c->jobs_head; j; ) {
        Job* n = j->next;
        free(j->url); free(j->temp_path); free(j);
        j = n;
    }
    for (ImageEntry* e = c->head; e; ) {
        ImageEntry* n = e->next;
        free(e->key);
        free(e->temp_path);
        if (e->tex) SDL_DestroyTexture(e->tex);
        free(e);
        e = n;
    }
    if (c->cond) SDL_DestroyCond(c->cond);
    if (c->lock) SDL_DestroyMutex(c->lock);
    free(c);
}

SDL_Texture* image_cache_get(ImageCache* c, const char* src,
                             int* w, int* h, int* status)
{
    int dummy_status;
    if (!status) status = &dummy_status;

    for (ImageEntry* e = c->head; e; e = e->next) {
        if (strcmp(e->key, src) != 0) continue;
        int st = SDL_AtomicGet(&e->state);
        if (st == ST_READY)   { *w = e->w; *h = e->h; *status = IMG_READY; return e->tex; }
        if (st == ST_FAILED)  { *status = IMG_FAILED;  return NULL; }
        if (st == ST_LOADING) { *status = IMG_LOADING; return NULL; }
        /* ST_DOWNLOADED: bytes are on disk; decode + upload on this (main)
         * thread now. */
        SDL_Texture* tex = decode_file(c->renderer, e->temp_path, &e->w, &e->h);
        if (tex) {
            e->tex = tex;
            SDL_AtomicSet(&e->state, ST_READY);
            *w = e->w; *h = e->h; *status = IMG_READY;
            return tex;
        }
        SDL_AtomicSet(&e->state, ST_FAILED);
        *status = IMG_FAILED;
        return NULL;
    }

    /* New entry. */
    ImageEntry* e = calloc(1, sizeof *e);
    e->key    = strdup(src);
    e->is_url = is_http_url(src);
    e->next   = c->head;
    c->head   = e;

    if (!e->is_url) {
        /* Local file: decode synchronously, exactly like before. */
        SDL_Texture* tex = decode_file(c->renderer, src, &e->w, &e->h);
        if (tex) {
            e->tex = tex;
            SDL_AtomicSet(&e->state, ST_READY);
            *w = e->w; *h = e->h; *status = IMG_READY;
            return tex;
        }
        SDL_AtomicSet(&e->state, ST_FAILED);
        *status = IMG_FAILED;
        return NULL;
    }

    /* Remote URL: reuse a prior on-disk download if present, else queue one. */
    e->temp_path = build_cache_path(src);
    if (file_exists(e->temp_path)) {
        SDL_AtomicSet(&e->state, ST_DOWNLOADED);
    } else {
        SDL_AtomicSet(&e->state, ST_LOADING);
        enqueue_job(c, e, src, e->temp_path);
    }
    *status = IMG_LOADING;
    return NULL;
}
